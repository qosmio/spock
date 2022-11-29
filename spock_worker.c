/*-------------------------------------------------------------------------
 *
 * spock.c
 * 		spock initialization and common functionality
 *
 * Copyright (c) 2021-2022, OSCG Partners, LLC
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, The Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"

#include "libpq/libpq-be.h"

#include "access/xact.h"

#include "commands/dbcommands.h"
#include "common/hashfn.h"

#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/procarray.h"

#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/lsyscache.h"
#include "replication/slot.h"
#include "replication/slot.h"

#include "pgstat.h"

#include "spock_sync.h"
#include "spock_worker.h"
#include <unistd.h>

#define SPOCK_CH_STATS	PGSTAT_STAT_PERMANENT_DIRECTORY "/spock_ch_stats.stat"
/*	Magic Number for spock stats - yyyymmddN */
#define SPOCK_CH_STATS_HEADER	202211251

typedef struct signal_worker_item
{
	Oid		subid;
	bool	kill;
} signal_worker_item;
static	List *signal_workers = NIL;

volatile sig_atomic_t	got_SIGTERM = false;

HTAB			   *SpockHash = NULL;
SpockContext	   *SpockCtx = NULL;
SpockWorker		   *MySpockWorker = NULL;
static uint16		MySpockWorkerGeneration;


static bool xacthook_signal_workers = false;
static bool xact_cb_installed = false;

#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;
#endif
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static void spock_shmem_shutdown(int code, Datum arg);
static void spock_worker_detach(bool crash);
static void wait_for_worker_startup(SpockWorker *worker,
									BackgroundWorkerHandle *handle);
static void signal_worker_xact_callback(XactEvent event, void *arg);
static uint32 spock_ch_stats_hash(const void *key, Size keysize);
static void load_ch_stats(void);

void
handle_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGTERM = true;

	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Find unused worker slot.
 *
 * The caller is responsible for locking.
 */
static int
find_empty_worker_slot(Oid dboid)
{
	int	i;

	Assert(LWLockHeldByMe(SpockCtx->lock));

	for (i = 0; i < SpockCtx->total_workers; i++)
	{
		if (SpockCtx->workers[i].worker_type == SPOCK_WORKER_NONE
		    || (SpockCtx->workers[i].crashed_at != 0
                && (SpockCtx->workers[i].dboid == dboid
                    || SpockCtx->workers[i].dboid == InvalidOid)))
			return i;
	}

	return -1;
}

/*
 * Register the spock worker proccess.
 *
 * Return the assigned slot number.
 */
int
spock_worker_register(SpockWorker *worker)
{
	BackgroundWorker	bgw;
	SpockWorker		*worker_shm;
	BackgroundWorkerHandle *bgw_handle;
	int					slot;
	int					next_generation;

	Assert(worker->worker_type != SPOCK_WORKER_NONE);

	LWLockAcquire(SpockCtx->lock, LW_EXCLUSIVE);

	slot = find_empty_worker_slot(worker->dboid);
	if (slot == -1)
	{
		LWLockRelease(SpockCtx->lock);
		elog(ERROR, "could not register spock worker: all background worker slots are already used");
	}

	worker_shm = &SpockCtx->workers[slot];

	/*
	 * Maintain a generation counter for worker registrations; see
	 * wait_for_worker_startup(...). The counter wraps around.
	 */
	if (worker_shm->generation == PG_UINT16_MAX)
		next_generation = 0;
	else
		next_generation = worker_shm->generation + 1;

	memcpy(worker_shm, worker, sizeof(SpockWorker));
	worker_shm->generation = next_generation;
	worker_shm->crashed_at = 0;
	worker_shm->proc = NULL;
	worker_shm->worker_type = worker->worker_type;

	LWLockRelease(SpockCtx->lock);

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags =	BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN,
			 EXTENSION_NAME);
	if (worker->worker_type == SPOCK_WORKER_MANAGER)
	{
		snprintf(bgw.bgw_function_name, BGW_MAXLEN,
				 "spock_manager_main");
		snprintf(bgw.bgw_name, BGW_MAXLEN,
				 "spock manager %u", worker->dboid);
	}
	else if (worker->worker_type == SPOCK_WORKER_SYNC)
	{
		snprintf(bgw.bgw_function_name, BGW_MAXLEN,
				 "spock_sync_main");
		snprintf(bgw.bgw_name, BGW_MAXLEN,
				 "spock sync %*s %u:%u", NAMEDATALEN - 37,
				 shorten_hash(NameStr(worker->worker.sync.relname), NAMEDATALEN - 37),
				 worker->dboid, worker->worker.sync.apply.subid);
	}
	else
	{
		snprintf(bgw.bgw_function_name, BGW_MAXLEN,
				 "spock_apply_main");
		snprintf(bgw.bgw_name, BGW_MAXLEN,
				 "spock apply %u:%u", worker->dboid,
				 worker->worker.apply.subid);
	}

	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	bgw.bgw_notify_pid = MyProcPid;
	bgw.bgw_main_arg = ObjectIdGetDatum(slot);

	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		worker_shm->crashed_at = GetCurrentTimestamp();
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("worker registration failed, you might want to increase max_worker_processes setting")));
	}

	wait_for_worker_startup(worker_shm, bgw_handle);

	return slot;
}

/*
 * This is our own version of WaitForBackgroundWorkerStartup where we wait
 * until worker actually attaches to our shmem.
 */
static void
wait_for_worker_startup(SpockWorker *worker,
						BackgroundWorkerHandle *handle)
{
	BgwHandleStatus status;
	int			rc;
	uint16		generation = worker->generation;

	Assert(handle != NULL);

	for (;;)
	{
		pid_t		pid = 0;

		CHECK_FOR_INTERRUPTS();

		if (got_SIGTERM)
		{
			elog(DEBUG1, "spock supervisor exiting on SIGTERM");
			proc_exit(0);
		}

		status = GetBackgroundWorkerPid(handle, &pid);

		if (status == BGWH_STARTED && spock_worker_running(worker))
		{
			elog(DEBUG2, "%s worker at slot %zu started with pid %d and attached to shmem",
				 spock_worker_type_name(worker->worker_type), (worker - &SpockCtx->workers[0]), pid);
			break;
		}
		if (status == BGWH_STOPPED)
		{
			/*
			 * The worker may have:
			 * - failed to launch after registration
			 * - launched then crashed/exited before attaching
			 * - launched, attached, done its work, detached cleanly and exited
			 *   before we got rescheduled
			 * - launched, attached, crashed and self-reported its crash, then
			 *   exited before we got rescheduled
			 *
			 * If it detached cleanly it will've set its worker type to
			 * SPOCK_WORKER_NONE, which it can't have been at entry, so we
			 * know it must've started, attached and cleared it.
			 *
			 * However, someone else might've grabbed the slot and re-used it
			 * and not exited yet, so if the worker type is not NONE we can't
			 * tell if it's our worker that's crashed or another worker that
			 * might still be running. We use a generation counter incremented
			 * on registration to tell the difference. If the generation
			 * counter has increased we know the our worker must've exited
			 * cleanly (setting the worker type back to NONE) or self-reported
			 * a crash (setting crashed_at), then the slot re-used by another
			 * manager.
			 */
			if (worker->worker_type != SPOCK_WORKER_NONE
				&& worker->generation == generation
				&& worker->crashed_at == 0)
			{
				/*
				 * The worker we launched (same generation) crashed before
				 * attaching to shmem so it didn't set crashed_at. Mark it
				 * crashed so the slot can be re-used.
				 */
				elog(DEBUG2, "%s worker at slot %zu exited prematurely",
					 spock_worker_type_name(worker->worker_type), (worker - &SpockCtx->workers[0]));
				worker->crashed_at = GetCurrentTimestamp();
			}
			else
			{
				/*
				 * Worker exited normally or self-reported a crash and may have already been
				 * replaced. Either way, we don't care, we're only looking for crashes before
				 * shmem attach.
				 */
				elog(DEBUG2, "%s worker at slot %zu exited before we noticed it started",
					 spock_worker_type_name(worker->worker_type), (worker - &SpockCtx->workers[0]));
			}
			break;
		}

		Assert(status == BGWH_NOT_YET_STARTED || status == BGWH_STARTED);

		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 1000L);

		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		ResetLatch(&MyProc->procLatch);
	}
}

/*
 * Cleanup function.
 *
 * Called on process exit.
 */
static void
spock_worker_on_exit(int code, Datum arg)
{
	spock_worker_detach(code != 0);
}

/*
 * Called on on_shmem_exit callback.
 */
static void
spock_shmem_shutdown(int code, Datum arg)
{
	save_ch_stats(code != 0);
}

/*
 * Attach the current master process to the SpockCtx.
 *
 * Called during worker startup to inform the master the worker
 * is ready and give it access to the worker's PGPROC.
 */
void
spock_worker_attach(int slot, SpockWorkerType type)
{
	Assert(slot >= 0);
	Assert(slot < SpockCtx->total_workers);

	/*
	 * Establish signal handlers. We must do this before unblocking the
	 * signals. The default SIGTERM handler of Postgres's background
	 * worker processes otherwise might throw a FATAL error, forcing us
	 * to exit while potentially holding a spinlock and/or corrupt
	 * shared memory.
	 */
	pqsignal(SIGTERM, handle_sigterm);

	/* Now safe to process signals */
	BackgroundWorkerUnblockSignals();

	MyProcPort = (Port *) calloc(1, sizeof(Port));

	LWLockAcquire(SpockCtx->lock, LW_EXCLUSIVE);

	before_shmem_exit(spock_worker_on_exit, (Datum) 0);

	MySpockWorker = &SpockCtx->workers[slot];
	Assert(MySpockWorker->proc == NULL);
	Assert(MySpockWorker->worker_type == type);
	MySpockWorker->proc = MyProc;
	MySpockWorkerGeneration = MySpockWorker->generation;

	elog(DEBUG2, "%s worker [%d] attaching to slot %d generation %hu",
		 spock_worker_type_name(type), MyProcPid, slot,
		 MySpockWorkerGeneration);

	/*
	 * So we can find workers in valgrind output, send a Valgrind client
	 * request to print to the Valgrind log.
	 */
	VALGRIND_PRINTF("SPOCK: spock worker %s (%s)\n",
		spock_worker_type_name(type),
		MyBgworkerEntry->bgw_name);

	LWLockRelease(SpockCtx->lock);

	/* Make it easy to identify our processes. */
	SetConfigOption("application_name", MyBgworkerEntry->bgw_name,
					PGC_BACKEND, PGC_S_OVERRIDE);

	/* Connect to database if needed. */
	if (MySpockWorker->dboid != InvalidOid)
	{
		MemoryContext oldcontext;

		BackgroundWorkerInitializeConnectionByOid(MySpockWorker->dboid,
												  InvalidOid
												  , 0 /* flags */
												  );


		StartTransactionCommand();
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		MyProcPort->database_name = pstrdup(get_database_name(MySpockWorker->dboid));
		MemoryContextSwitchTo(oldcontext);
		CommitTransactionCommand();
	}
}

/*
 * Detach the current master process from the SpockCtx.
 *
 * Called during master worker exit.
 */
static void
spock_worker_detach(bool crash)
{
	/* Nothing to detach. */
	if (MySpockWorker == NULL)
		return;

	LWLockAcquire(SpockCtx->lock, LW_EXCLUSIVE);

	Assert(MySpockWorker->proc = MyProc);
	Assert(MySpockWorker->generation == MySpockWorkerGeneration);
	MySpockWorker->proc = NULL;

	elog(LOG, "%s worker [%d] at slot %zu generation %hu %s",
		 spock_worker_type_name(MySpockWorker->worker_type),
		 MyProcPid, MySpockWorker - &SpockCtx->workers[0],
		 MySpockWorkerGeneration,
		 crash ? "exiting with error" : "detaching cleanly");

	VALGRIND_PRINTF("SPOCK: worker detaching, unclean=%d\n",
		crash);

	/*
	 * If we crashed we need to report it.
	 *
	 * The crash logic only works because all of the workers are attached
	 * to shmem and the serious crashes that we can't catch here cause
	 * postmaster to restart whole server killing all our workers and cleaning
	 * shmem so we start from clean state in that scenario.
	 *
	 * It's vital NOT to clear or change the generation field here; see
	 * wait_for_worker_startup(...).
	 */
	if (crash)
	{
		MySpockWorker->crashed_at = GetCurrentTimestamp();

		/* Manager crash, make sure supervisor notices. */
		if (MySpockWorker->worker_type == SPOCK_WORKER_MANAGER)
			SpockCtx->subscriptions_changed = true;
	}
	else
	{
		/* Worker has finished work, clean up its state from shmem. */
		MySpockWorker->worker_type = SPOCK_WORKER_NONE;
		MySpockWorker->dboid = InvalidOid;
	}

	MySpockWorker = NULL;

	LWLockRelease(SpockCtx->lock);
}

/*
 * Find the manager worker for given database.
 */
SpockWorker *
spock_manager_find(Oid dboid)
{
	int i;

	Assert(LWLockHeldByMe(SpockCtx->lock));

	for (i = 0; i < SpockCtx->total_workers; i++)
	{
		if (SpockCtx->workers[i].worker_type == SPOCK_WORKER_MANAGER &&
			dboid == SpockCtx->workers[i].dboid)
			return &SpockCtx->workers[i];
	}

	return NULL;
}

/*
 * Find the apply worker for given subscription.
 */
SpockWorker *
spock_apply_find(Oid dboid, Oid subscriberid)
{
	int i;

	Assert(LWLockHeldByMe(SpockCtx->lock));

	for (i = 0; i < SpockCtx->total_workers; i++)
	{
		SpockWorker	   *w = &SpockCtx->workers[i];

		if (w->worker_type == SPOCK_WORKER_APPLY &&
			dboid == w->dboid &&
			subscriberid == w->worker.apply.subid)
			return w;
	}

	return NULL;
}

/*
 * Find all apply workers for given database.
 */
List *
spock_apply_find_all(Oid dboid)
{
	int			i;
	List	   *res = NIL;

	Assert(LWLockHeldByMe(SpockCtx->lock));

	for (i = 0; i < SpockCtx->total_workers; i++)
	{
		if (SpockCtx->workers[i].worker_type == SPOCK_WORKER_APPLY &&
			dboid == SpockCtx->workers[i].dboid)
			res = lappend(res, &SpockCtx->workers[i]);
	}

	return res;
}

/*
 * Find the sync worker for given subscription and table
 */
SpockWorker *
spock_sync_find(Oid dboid, Oid subscriberid, const char *nspname, const char *relname)
{
	int i;

	Assert(LWLockHeldByMe(SpockCtx->lock));

	for (i = 0; i < SpockCtx->total_workers; i++)
	{
		SpockWorker *w = &SpockCtx->workers[i];
		if (w->worker_type == SPOCK_WORKER_SYNC && dboid == w->dboid &&
			subscriberid == w->worker.apply.subid &&
			strcmp(NameStr(w->worker.sync.nspname), nspname) == 0 &&
			strcmp(NameStr(w->worker.sync.relname), relname) == 0)
			return w;
	}

	return NULL;
}


/*
 * Find the sync worker for given subscription
 */
List *
spock_sync_find_all(Oid dboid, Oid subscriberid)
{
	int			i;
	List	   *res = NIL;

	Assert(LWLockHeldByMe(SpockCtx->lock));

	for (i = 0; i < SpockCtx->total_workers; i++)
	{
		SpockWorker *w = &SpockCtx->workers[i];
		if (w->worker_type == SPOCK_WORKER_SYNC && dboid == w->dboid &&
			subscriberid == w->worker.apply.subid)
			res = lappend(res, w);
	}

	return res;
}

/*
 * Get worker based on slot
 */
SpockWorker *
spock_get_worker(int slot)
{
	Assert(LWLockHeldByMe(SpockCtx->lock));
	return &SpockCtx->workers[slot];
}

/*
 * Is the worker running?
 */
bool
spock_worker_running(SpockWorker *worker)
{
	return worker && worker->proc;
}

void
spock_worker_kill(SpockWorker *worker)
{
	Assert(LWLockHeldByMe(SpockCtx->lock));
	if (spock_worker_running(worker))
	{
		elog(DEBUG2, "killing spock %s worker [%d] at slot %zu",
			 spock_worker_type_name(worker->worker_type),
			 worker->proc->pid, (worker - &SpockCtx->workers[0]));
		kill(worker->proc->pid, SIGTERM);
	}
}

static void
signal_worker_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT && xacthook_signal_workers)
	{
		SpockWorker	   *w;
		ListCell	   *l;

		LWLockAcquire(SpockCtx->lock, LW_EXCLUSIVE);

		foreach (l, signal_workers)
		{
			signal_worker_item *item = (signal_worker_item *) lfirst(l);

			w = spock_apply_find(MyDatabaseId, item->subid);
			if (item->kill)
				spock_worker_kill(w);
			else if (spock_worker_running(w))
			{
				w->worker.apply.sync_pending = true;
				SetLatch(&w->proc->procLatch);
			}
		}

		SpockCtx->subscriptions_changed = true;

		/* Signal the manager worker, if there's one */
		w = spock_manager_find(MyDatabaseId);
		if (spock_worker_running(w))
			SetLatch(&w->proc->procLatch);

		/* and signal the supervisor, for good measure */
		if (SpockCtx->supervisor)
			SetLatch(&SpockCtx->supervisor->procLatch);

		LWLockRelease(SpockCtx->lock);

		list_free_deep(signal_workers);
		signal_workers = NIL;

		xacthook_signal_workers = false;
	}
}

/*
 * Enqueue signal for supervisor/manager at COMMIT.
 */
void
spock_subscription_changed(Oid subid, bool kill)
{
	if (!xact_cb_installed)
	{
		RegisterXactCallback(signal_worker_xact_callback, NULL);
		xact_cb_installed = true;
	}

	if (OidIsValid(subid))
	{
		MemoryContext	oldcxt;
		signal_worker_item *item;

		oldcxt = MemoryContextSwitchTo(TopTransactionContext);

		item = palloc(sizeof(signal_worker_item));
		item->subid = subid;
		item->kill = kill;

		signal_workers = lappend(signal_workers, item);

		MemoryContextSwitchTo(oldcxt);
	}

	xacthook_signal_workers = true;
}

static size_t
worker_shmem_size(int nworkers)
{
	return offsetof(SpockContext, workers) +
		sizeof(SpockWorker) * nworkers;
}

/*
 * Requests any additional shared memory required for spock.
 */
static void
spock_worker_shmem_request(void)
{
	int			nworkers;

#if PG_VERSION_NUM >= 150000
	if (prev_shmem_request_hook != NULL)
		prev_shmem_request_hook();
#else
	Assert(process_shared_preload_libraries_in_progress);
#endif

	/*
	 * This is cludge for Windows (Postgres des not define the GUC variable
	 * as PGDDLIMPORT)
	 */
	nworkers = atoi(GetConfigOptionByName("max_worker_processes", NULL,
										  false));

	/* Allocate enough shmem for the worker limit ... */
	RequestAddinShmemSpace(worker_shmem_size(nworkers));

	/*
	 * We'll need to be able to take exclusive locks so only one per-db backend
	 * tries to allocate or free blocks from this array at once.  There won't
	 * be enough contention to make anything fancier worth doing.
	 */
	RequestNamedLWLockTranche("spock", 1);
}

/*
 * Init shmem needed for workers.
 */
static void
spock_worker_shmem_startup(void)
{
	bool        found;
	int			nworkers;
	HASHCTL		hctl;

	if (prev_shmem_startup_hook != NULL)
		prev_shmem_startup_hook();

	/*
	 * This is kludge for Windows (Postgres does not define the GUC variable
	 * as PGDLLIMPORT)
	 */
	nworkers = atoi(GetConfigOptionByName("max_worker_processes", NULL,
										  false));

	SpockCtx = NULL;
	 /* avoid possible race-conditions, when initializing the shared memory. */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	/* Init signaling context for the various processes. */
	SpockCtx = ShmemInitStruct("spock_context",
								   worker_shmem_size(nworkers), &found);

	if (!found)
	{
		SpockCtx->lock = &(GetNamedLWLockTranche("spock"))->lock;
		SpockCtx->supervisor = NULL;
		SpockCtx->subscriptions_changed = false;
		SpockCtx->total_workers = nworkers;
		memset(SpockCtx->workers, 0,
			   sizeof(SpockWorker) * SpockCtx->total_workers);
	}

	memset(&hctl, 0, sizeof(hctl));
	hctl.keysize = sizeof(spockHashKey);
	hctl.entrysize = sizeof(spockStatsEntry);
	hctl.hash = spock_ch_stats_hash;
	SpockHash = ShmemInitHash("spock channel stats hash",
							  max_replication_slots,
							  max_replication_slots,
							  &hctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_FIXED_SIZE);

	if (!IsUnderPostmaster)
		on_shmem_exit(spock_shmem_shutdown, 0);

	load_ch_stats();
	LWLockRelease(AddinShmemInitLock);
}

/*
 * save_ch_stats:
 *
 * Save the stats to the file named SPOCK_CH_STATS (defined above). This
 * function is called during the on_exit_shmem callback is executed.
 *
 * The format of this file is as follows:
 * SPOCK_CH_STATS_HEADER  (uint32) - header, a unique number to differentiate
 * 		version of this file.
 * SPOCK_VERSION_NUM (uint32) - Spock Version
 * N_ENTRIES (uint32) - num of entries being written
 * ENTRY (spockStatsEntry) - one liner per entry of hashtable.
 */
void
save_ch_stats(bool crash)
{
	FILE	   *file;
	HASH_SEQ_STATUS		hash_seq;
	spockStatsEntry	   *entry;
	uint32				header;
	uint32				spock_version;
	uint32				nenteries;

	/* don't try to save during crash */
	if (crash)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!SpockCtx || !SpockHash)
		return;

	header = SPOCK_CH_STATS_HEADER;
	spock_version = SPOCK_VERSION_NUM;
	file = AllocateFile(SPOCK_CH_STATS ".tmp", PG_BINARY_W);
	if (file == NULL)
		goto error;

	if (fwrite(&header, sizeof(uint32), 1, file) != 1)
		goto error;
	if (fwrite(&spock_version, sizeof(uint32), 1, file) != 1)
		goto error;
	nenteries = hash_get_num_entries(SpockHash);
	if (fwrite(&nenteries, sizeof(int32), 1, file) != 1)
		goto error;

	hash_seq_init(&hash_seq, SpockHash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (fwrite(entry, sizeof(spockStatsEntry), 1, file) != 1)
		{
			/* note: we assume hash_seq_term won't change errno */
			hash_seq_term(&hash_seq);
			goto error;
		}
	}

	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}

	/*
	 * Rename file into place, so we atomically replace any old one.
	 */
	(void) durable_rename(SPOCK_CH_STATS ".tmp", SPOCK_CH_STATS, LOG);
	return;
error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					SPOCK_CH_STATS ".tmp")));
	if (file)
		FreeFile(file);
	unlink(SPOCK_CH_STATS ".tmp");
}

/*
 * load_ch_stats:
 *
 * Load the stats from file (SPOCK_CH_STATS) and create entries for it in the
 * hash table. This function is called from shmem startup and it should not be
 * called from anywhere else.
 *
 * Note that there are no locks/mutex etc because at this point there should not
 * be any other process active.
 */
static void
load_ch_stats(void)
{
	FILE	   *file;
	uint32		header;
	uint32		spock_version;
	uint32		nentries;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!SpockCtx || !SpockHash)
		return;

	file = AllocateFile(SPOCK_CH_STATS, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
			goto read_error;

		/* No existing persisted stats file, so we're done */
		return;
	}

	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		fread(&spock_version, sizeof(uint32), 1, file) != 1 ||
		fread(&nentries, sizeof(int32), 1, file) != 1)
		goto read_error;

	if (header != SPOCK_CH_STATS_HEADER ||
		spock_version != SPOCK_VERSION_NUM)
		goto data_error;

	for (int i = 0; i < nentries; i++)
	{
		spockStatsEntry	temp;
		spockStatsEntry *entry;
		bool			found;

		if (fread(&temp, sizeof(spockStatsEntry), 1, file) != 1)
			goto read_error;

		/* Add the entry in the hashtable */
		entry = (spockStatsEntry *) hash_search(SpockHash, &temp.key,
												HASH_ENTER, &found);
		if (!found)
		{
			memset(&entry->counters, 0, sizeof(spockCounters));
			SpinLockInit(&entry->mutex);
		}
		/* copy counters to the hashtable */
		entry->counters = temp.counters;
	}

	FreeFile(file);
	return;

read_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read file \"%s\": %m",
					SPOCK_CH_STATS)));
	goto fail;
data_error:
	ereport(LOG,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("ignoring invalid data in file \"%s\"",
					SPOCK_CH_STATS)));
	goto fail;
fail:
	if (file)
		FreeFile(file);
}

/*
 * Hash functions for spock counters.
 */
static uint32
spock_ch_stats_hash(const void *key, Size keysize)
{
	const spockHashKey *k = (const spockHashKey *) key;

	Assert(keysize == sizeof(spockHashKey));

	return hash_uint32((uint32) k->dboid) ^
		hash_uint32((uint32) k->relid);
}

/*
 * Request shmem resources for our worker management.
 */
void
spock_worker_shmem_init(void)
{
	/*
	 * Whether this is a first startup or crash recovery, we'll be re-initing
	 * the bgworkers.
	 */
	SpockCtx = NULL;
	MySpockWorker = NULL;

#if PG_VERSION_NUM < 150000
	spock_worker_shmem_request();
#else
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = spock_worker_shmem_request;
#endif
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = spock_worker_shmem_startup;
}

const char *
spock_worker_type_name(SpockWorkerType type)
{
	switch (type)
	{
		case SPOCK_WORKER_NONE: return "none";
		case SPOCK_WORKER_MANAGER: return "manager";
		case SPOCK_WORKER_APPLY: return "apply";
		case SPOCK_WORKER_SYNC: return "sync";
		default: Assert(false); return NULL;
	}
}

void
handle_sub_counters(Relation relation, spockStatsType typ, int ntup)
{
	bool found = false;
	SpockSubscription *sub = get_subscription(MyApplyWorker->subid);
	spockHashKey key;
	spockCounters *counters;
	spockStatsEntry *entry;

	memset(&key, 0, sizeof(spockHashKey));
	key.dboid = MyDatabaseId;
	key.relid = RelationGetRelid(relation);

	entry = (spockStatsEntry *) hash_search(SpockHash, &key,
										HASH_ENTER, &found);
	if (!found)
	{
		/* New stats entry */
		counters = &entry->counters;
		memset(counters, 0, sizeof(spockCounters));
		counters->last_reset = GetCurrentTimestamp();
		SpinLockInit(&entry->mutex);
	}

	counters = &entry->counters;
	SpinLockAcquire(&entry->mutex);
	entry->nodeid = sub->target_if->nodeid;
	strncpy(entry->slot_name, sub->slot_name, NAMEDATALEN);
	strncpy(entry->schemaname,
			get_namespace_name(RelationGetNamespace(relation)),
			NAMEDATALEN);
	strncpy(entry->relname, RelationGetRelationName(relation), NAMEDATALEN);
	switch(typ)
	{
		case INSERT_STATS:
				counters->n_tup_ins += ntup;
			break;
		case UPDATE_STATS:
				counters->n_tup_upd += ntup;
			break;
		case DELETE_STATS:
				counters->n_tup_del += ntup;
			break;
		default:
			elog(ERROR, "invalid stat type (%u) specified", typ);
	}
	SpinLockRelease(&entry->mutex);
}

void
handle_pr_counters(Relation relation, Oid nodeid, spockStatsType typ, int ntup)
{
	bool found = false;
	spockHashKey key;
	spockCounters *counters;
	spockStatsEntry *entry;

	memset(&key, 0, sizeof(spockHashKey));
	key.dboid = MyDatabaseId;
	key.relid = RelationGetRelid(relation);

	entry = (spockStatsEntry *) hash_search(SpockHash, &key,
										HASH_ENTER, &found);
	if (!found)
	{
		/* New stats entry */
		counters = &entry->counters;
		memset(counters, 0, sizeof(spockCounters));
		counters->last_reset = GetCurrentTimestamp();
		SpinLockInit(&entry->mutex);
	}

	counters = &entry->counters;
	SpinLockAcquire(&entry->mutex);
	entry->nodeid = nodeid;
	memset(entry->slot_name, 0, NAMEDATALEN);
	strncpy(entry->schemaname,
			get_namespace_name(RelationGetNamespace(relation)),
			NAMEDATALEN);
	strncpy(entry->relname, RelationGetRelationName(relation), NAMEDATALEN);
	switch(typ)
	{
		case INSERT_STATS:
				counters->n_tup_ins += ntup;
			break;
		case UPDATE_STATS:
				counters->n_tup_upd += ntup;
			break;
		case DELETE_STATS:
				counters->n_tup_del += ntup;
			break;
		default:
			elog(ERROR, "invalid stat type (%u) specified", typ);
	}
	SpinLockRelease(&entry->mutex);
}
