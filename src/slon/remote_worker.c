/*-------------------------------------------------------------------------
 * remote_worker.c
 *
 *	Implementation of the thread processing remote events.
 *
 *	Copyright (c) 2003-2004, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *
 *	$Id: remote_worker.c,v 1.5 2004-02-27 06:03:38 wieck Exp $
 *-------------------------------------------------------------------------
 */


#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>

#include "libpq-fe.h"
#include "c.h"

#include "slon.h"


/* ----------
 * Local definitions
 * ----------
 */

/*
 * Internal message types
 */
#define	WMSG_EVENT		0
#define WMSG_WAKEUP		1
#define WMSG_CONFIRM	2


/*
 * Message structure resulting from a remote event
 */
typedef struct SlonWorkMsg_event_s SlonWorkMsg_event;
struct SlonWorkMsg_event_s {
	int			msg_type;
	SlonWorkMsg_event *prev;
	SlonWorkMsg_event *next;

	int			event_provider;

	int			ev_origin;
	int64		ev_seqno;
	char	   *ev_timestamp_c;
	char	   *ev_minxid_c;
	char	   *ev_maxxid_c;
	char	   *ev_xip;
	char	   *ev_type;
	char	   *ev_data1;
	char	   *ev_data2;
	char	   *ev_data3;
	char	   *ev_data4;
	char	   *ev_data5;
	char	   *ev_data6;
	char	   *ev_data7;
	char	   *ev_data8;
	char		raw_data[1];
};


/*
 * Message structure resulting from a remote confirm
 */
typedef struct SlonWorkMsg_confirm_s SlonWorkMsg_confirm;
struct SlonWorkMsg_confirm_s {
	int			msg_type;
	SlonWorkMsg_confirm *prev;
	SlonWorkMsg_confirm *next;

	int			con_origin;
	int			con_received;
	int64		con_seqno;
	char		con_timestamp_c[64];
};


/*
 * Generic message header
 */
struct SlonWorkMsg_s {
	int			msg_type;
	SlonWorkMsg *prev;
	SlonWorkMsg *next;
};


typedef struct ProviderInfo_s ProviderInfo;
typedef struct ProviderSet_s ProviderSet;
typedef struct WorkerGroupData_s WorkerGroupData;
typedef struct WorkerGroupLine_s WorkerGroupLine;


struct ProviderSet_s {
	int			set_id;
	int			sub_forward;

	ProviderSet *prev;
	ProviderSet *next;
};


typedef enum {
	SLON_WG_IDLE,
	SLON_WG_BUSY,
	SLON_WG_DONE,
	SLON_WG_EXIT,
	SLON_WG_ABORT
} WorkGroupStatus;


typedef enum {
	SLON_WGLC_ACTION,
	SLON_WGLC_DONE,
	SLON_WGLC_ERROR
} WorkGroupLineCode;


struct ProviderInfo_s {
	int			no_id;
	char	   *pa_conninfo;
	int			pa_connretry;
	SlonConn   *conn;

	WorkerGroupData *wd;

	pthread_t		helper_thread;
	pthread_mutex_t	helper_lock;
	pthread_cond_t	helper_cond;
	WorkGroupStatus	helper_status;
	SlonDString		helper_qualification;

	ProviderSet *set_head;
	ProviderSet *set_tail;

	ProviderInfo *prev;
	ProviderInfo *next;
};


struct WorkerGroupData_s {
	SlonNode		   *node;

	char			  **tab_fqname;
	int					tab_fqname_size;

	ProviderInfo	   *provider_head;
	ProviderInfo	   *provider_tail;

	pthread_mutex_t		workdata_lock;
	WorkGroupStatus		workgroup_status;

	pthread_cond_t		repldata_cond;
	WorkerGroupLine	   *repldata_head;
	WorkerGroupLine	   *repldata_tail;

	pthread_cond_t		linepool_cond;
	WorkerGroupLine	   *linepool_head;
	WorkerGroupLine	   *linepool_tail;
};


struct WorkerGroupLine_s {
	WorkGroupLineCode	code;
	ProviderInfo	   *provider;
	SlonDString			data;

	WorkerGroupLine	   *prev;
	WorkerGroupLine	   *next;
};


/*
 * Global status for all remote worker threads, remembering the
 * last seen confirmed sequence number.
 */
struct node_confirm_status {
	int			con_origin;
	int			con_received;
	int64		con_seqno;

	struct node_confirm_status	*prev;
	struct node_confirm_status	*next;
};
static struct node_confirm_status  *node_confirm_head = NULL;
static struct node_confirm_status  *node_confirm_tail = NULL;
pthread_mutex_t						node_confirm_lock = PTHREAD_MUTEX_INITIALIZER;


/* ----------
 * Local functions
 * ----------
 */
static int	query_execute(SlonNode *node, PGconn *dbconn, 
					SlonDString *dsp, int expected_rc);
static void	query_append_event(SlonDString *dsp, 
					SlonWorkMsg_event *event);
static void	store_confirm_forward(SlonNode *node, SlonConn *conn,
					SlonWorkMsg_confirm *confirm);
static int	copy_set(SlonNode *node, SlonConn *local_conn, int set_id);
static int	sync_event(SlonNode *node, SlonConn *local_conn,
					WorkerGroupData *wd,SlonWorkMsg_event *event);
static void	*sync_helper(void *cdata);


static void
adjust_provider_info(SlonNode *node, WorkerGroupData *wd, int cleanup)
{
	ProviderInfo   *provider;
	ProviderInfo   *provnext;
	ProviderSet	   *pset;
	SlonNode	   *rtcfg_node;
	SlonSet		   *rtcfg_set;
	int				i;

	slon_log(SLON_DEBUG2, "remoteWorderThread_%d: "
			"update provider configuration\n",
			node->no_id);

	/*
	 * The runtime configuration has changed. 
	 */

	/*
	 * Step 1. 
	 *
	 *	Remove all sets from the providers.
	 */
	for (provider = wd->provider_head; provider; provider = provider->next)
	{
		/*
		 * We create a lock here and keep it until we made
		 * our final decision about what to do with the helper thread.
		 */
		pthread_mutex_lock(&(provider->helper_lock));

		while ((pset = provider->set_head) != NULL)
		{
			DLLIST_REMOVE(provider->set_head, provider->set_tail,
					pset);
			free(pset);
		}
	}

	/*
	 * Step 2.
	 *
	 *	Add all currently replicated sets (back) to the providers
	 *	adding new providers as necessary. This step is skippen in
	 *	cleanup mode, causing all providers to become obsolete
	 *	and thus the whole provider info extinct.
	 */
	if (!cleanup)
	{
		for (rtcfg_set = rtcfg_set_list_head; rtcfg_set; 
				rtcfg_set = rtcfg_set->next)
		{
			if (rtcfg_set->sub_provider >= 0 &&
				rtcfg_set->sub_active)
			{
				/*
				 * We need to replicate this set. Find or add
				 * the provider to our in-thread data.
				 */
				for (provider = wd->provider_head; provider;
						provider = provider->next)
				{
					if (provider->no_id == rtcfg_set->sub_provider)
						break;
				}
				if (provider == NULL)
				{
					/*
					 * No provider entry found. Create a new one.
					 */
					provider = (ProviderInfo *)
							malloc(sizeof(ProviderInfo));
					memset(provider, 0, sizeof(ProviderInfo));
					provider->no_id = rtcfg_set->sub_provider;
					provider->wd = wd;

					/*
					 * Also create a helper thread for this
					 * provider, which will actually run the
					 * log data selection for us.
					 */
					pthread_mutex_init(&(provider->helper_lock), NULL);
					pthread_mutex_lock(&(provider->helper_lock));
					pthread_cond_init(&(provider->helper_cond), NULL);
					dstring_init(&(provider->helper_qualification));
					provider->helper_status = SLON_WG_IDLE;
					if (pthread_create(&(provider->helper_thread), NULL,
							sync_helper, (void *)provider) != 0)
					{
						slon_log(SLON_FATAL, "remoteWorkerThread_%d: ",
								"pthread_create() - %s\n",
								node->no_id, strerror(errno));
						slon_abort();
					}
					slon_log(SLON_DEBUG1, "remoteWorkerThread_%d: "
							"helper thread for provider %d created\n",
							node->no_id, provider->no_id);

					/*
					 * Add more workgroup data lines to the pool.
					 */
					for (i = 0; i < SLON_WORKLINES_PER_HELPER; i++)
					{
						WorkerGroupLine	   *line;

						line = (WorkerGroupLine *)malloc(sizeof(WorkerGroupLine));
						memset(line, 0, sizeof(WorkerGroupLine));
						dstring_init(&(line->data));
						DLLIST_ADD_TAIL(wd->linepool_head, wd->linepool_tail,
								line);
					}

					/*
					 * Add the provider to our work group
					 */
					DLLIST_ADD_TAIL(wd->provider_head, wd->provider_tail,
							provider);
					
					/*
					 * Copy the runtime configurations conninfo
					 * into the provider info.
					 */
					rtcfg_node = rtcfg_findNode(provider->no_id);
					if (rtcfg_node != NULL)
					{
						provider->pa_connretry = rtcfg_node->pa_connretry;
						if (rtcfg_node->pa_conninfo != NULL)
							provider->pa_conninfo =
									strdup(rtcfg_node->pa_conninfo);
					}
				}

				/*
				 * Add the set to the list of sets we get
				 * from this provider.
				 */
				pset = (ProviderSet *)
						malloc(sizeof(ProviderSet));
				memset(pset, 0, sizeof(ProviderSet));
				pset->set_id      = rtcfg_set->set_id;
				pset->sub_forward = rtcfg_set->sub_forward;

				DLLIST_ADD_TAIL(provider->set_head,
						provider->set_tail, pset);

				slon_log(SLON_DEBUG2, "remoteWorkerThread_%d: "
						"added active set %d to provider %d\n",
						node->no_id, pset->set_id, provider->no_id);
			}
		}
	}

	/*
	 * Step 3.
	 *
	 *	Remove all providers that we don't need any more.
	 */
	for (provider = wd->provider_head; provider; provider = provnext)
	{
		SlonNode	   *rtcfg_node;

		provnext = provider->next;

		/*
		 * If the list of currently replicated sets we receive from
		 * this provider is empty, we don't need to maintain a
		 * connection to it.
		 */
		if (provider->set_head == NULL)
		{
			/*
			 * Tell this helper thread to exit, join him and destroy
			 * thread related data.
			 */
			provider->helper_status = SLON_WG_EXIT;
			pthread_cond_signal(&(provider->helper_cond));
			pthread_mutex_unlock(&(provider->helper_lock));
			pthread_join(provider->helper_thread, NULL);
			pthread_cond_destroy(&(provider->helper_cond));
			pthread_mutex_destroy(&(provider->helper_lock));

			slon_log(SLON_DEBUG1, "remoteWorkerThread_%d: "
					"helper thread for provider %d terminated\n",
					node->no_id, provider->no_id);

			/*
			 * Disconnect from the database.
			 */
			if (provider->conn != NULL)
			{
				slon_log(SLON_DEBUG1, "remoteWorkerThread_%d: "
						"disconnecting from data provider %d\n",
						node->no_id, provider->no_id);
				slon_disconnectdb(provider->conn);
			}

			/*
			 * Free other resources
			 */
			if (provider->pa_conninfo != NULL)
				free(provider->pa_conninfo);
			DLLIST_REMOVE(wd->provider_head, wd->provider_tail, provider);
			dstring_free(&(provider->helper_qualification));
			free(provider);

			continue;
		}

		/*
		 * If the connection info has changed, we have to reconnect.
		 */
		rtcfg_node = rtcfg_findNode(provider->no_id);
		if (rtcfg_node == NULL || rtcfg_node->pa_conninfo == NULL ||
				provider->pa_conninfo == NULL ||
				strcmp(provider->pa_conninfo, rtcfg_node->pa_conninfo) != 0)
		{
			if (provider->conn != NULL)
			{
				slon_log(SLON_DEBUG1, "remoteWorkerThread_%d: "
						"disconnecting from data provider %d\n",
						node->no_id, provider->no_id);
				slon_disconnectdb(provider->conn);
				provider->conn = NULL;
			}
			if (provider->pa_conninfo != NULL)
				free(provider->pa_conninfo);
			if (rtcfg_node->pa_conninfo == NULL)
				provider->pa_conninfo = NULL;
			else
				provider->pa_conninfo = strdup(rtcfg_node->pa_conninfo);
		}

		/*
		 * Unlock the helper thread ... he should now go and wait
		 * for work.
		 */
		pthread_mutex_unlock(&(provider->helper_lock));
	}
}


/* ----------
 * slon_remoteWorkerThread
 *
 *	Listen for events on the local database connection. This means,
 *	events generated by the local node only.
 * ----------
 */
void *
remoteWorkerThread_main(void *cdata)
{
	SlonNode   *node = (SlonNode *)cdata;
	WorkerGroupData	   *wd;
	SlonConn   *local_conn;
	PGconn	   *local_dbconn;
	SlonDString	query1;
	SlonDString	query2;
	SlonWorkMsg	   *msg;
	SlonWorkMsg_event *event;
	int			check_config = true;
	int64		curr_config = -1;
	char		seqbuf[64];

	slon_log(SLON_DEBUG1,
			"remoteWorkerThread_%d: thread starts\n", 
			node->no_id);
	/*
	 * Initialize local data
	 */
	wd = (WorkerGroupData *)malloc(sizeof(WorkerGroupData));
	memset(wd, 0, sizeof(WorkerGroupData));

	pthread_mutex_init(&(wd->workdata_lock), NULL);
	pthread_cond_init(&(wd->repldata_cond), NULL);
	pthread_cond_init(&(wd->linepool_cond), NULL);
	pthread_mutex_lock(&(wd->workdata_lock));
	wd->workgroup_status = SLON_WG_IDLE;
	wd->node = node;

	wd->tab_fqname_size = 1024;
	wd->tab_fqname = (char **)malloc(sizeof(char *) * wd->tab_fqname_size);
	memset(wd->tab_fqname, 0, sizeof(char *) * wd->tab_fqname_size);

	dstring_init(&query1);
	dstring_init(&query2);

	/*
	 * Connect to the local database
	 */
	if ((local_conn = slon_connectdb(rtcfg_conninfo, "remote_worker")) == NULL)
		slon_abort();
	local_dbconn = local_conn->dbconn;

	/*
	 * Put the connection into replication mode.
	 */
	slon_mkquery(&query1,
			"select %s.setSessionRole('_%s', 'slon'); ",
			rtcfg_namespace, rtcfg_cluster_name);
	if (query_execute(node, local_dbconn, &query1, PGRES_TUPLES_OK) < 0)
		slon_abort();

	/*
	 * Work until shutdown or node destruction
	 */
	while (true)
	{
		/*
		 * If we got the special WMSG_WAKEUP, check the current runmode
		 * of the scheduler and the status of our node.
		 */
		if (check_config)
		{
			if (sched_get_status() != SCHED_STATUS_OK)
				break;

			rtcfg_lock();
			if (!node->no_active)
				break;
			if (node->worker_status != SLON_TSTAT_RUNNING)
				break;

			if (curr_config != rtcfg_seq_get())
			{
				adjust_provider_info(node, wd, false);
				curr_config = rtcfg_seq_get();
			}

			rtcfg_unlock();

			check_config = false;
		}

		/*
		 * Receive the next message from the queue. If there is no
		 * one present, wait on the condition variable.
		 */
		pthread_mutex_lock(&(node->message_lock));
		if (node->message_head == NULL)
		{
			pthread_cond_wait(&(node->message_cond), &(node->message_lock));
			if (node->message_head == NULL)
			{
				slon_log(SLON_FATAL,
						"remoteWorkerThread_%d: got message "
						"condition but queue is empty\n",
						node->no_id);
				slon_abort();
			}
		}
		msg = node->message_head;
		DLLIST_REMOVE(node->message_head, node->message_tail, msg);
		pthread_mutex_unlock(&(node->message_lock));

		/*
		 * Process WAKEUP messages by simply setting the check_config
		 * flag.
		 */
		if (msg->msg_type == WMSG_WAKEUP)
		{
			free(msg);
			check_config = true;
			continue;
		}

		/*
		 * Process confirm messages.
		 */
		if (msg->msg_type == WMSG_CONFIRM)
		{
			store_confirm_forward(node, local_conn,
					(SlonWorkMsg_confirm *)msg);
			free(msg);
			continue;
		}

		/*
		 * This must be an event message then.
		 */
		if (msg->msg_type != WMSG_EVENT)
		{
			slon_log(SLON_FATAL,
					"remoteWorkerThread_%d: unknown WMSG type %d\n",
					node->no_id, msg->msg_type);
			slon_abort();
		}

		event = (SlonWorkMsg_event *)msg;
		sprintf(seqbuf, "%lld", event->ev_seqno);

		/*
		 * Construct the queries to begin a transaction, notify on
		 * the event and confirm relations, insert the event into our
		 * local sl_event table and confirm it in our local sl_confirm
		 * table. When this transaction commits, every other remote
		 * node listening for events with us as a provider will pick
		 * up the news.
		 */
		slon_mkquery(&query1, "begin transaction; ");
		query_append_event(&query1, event);
		
		/*
		 * Event type specific processing
		 */
		if (strcmp(event->ev_type, "SYNC") == 0)
		{
			int		seconds;
			int		rc;

			/*
			 * SYNC event
			 */
			while (true)
			{
				/*
				 * Execute the forwarding and notify stuff, but
				 * do not commit the transaction yet.
				 */
				if (query_execute(node, local_dbconn, &query1, 
						PGRES_COMMAND_OK) < 0)
					slon_abort();

				/*
				 * Process the sync and apply the replication data.
				 * If successful, exit this loop and commit the transaction.
				 */
				seconds = sync_event(node, local_conn, wd, event);
				if (seconds == 0)
				{
					rc = SCHED_STATUS_OK;
					break;
				}
				
				/*
				 * Something went wrong. Rollback and try again
				 * after the specified timeout.
				 */
				slon_mkquery(&query2, "rollback transaction");
				if (query_execute(node, local_dbconn, &query2,
						PGRES_COMMAND_OK) < 0)
					slon_abort();

				if ((rc = sched_msleep(node, seconds * 1000)) != SCHED_STATUS_OK)
					break;
			}
			if (rc != SCHED_STATUS_OK)
				break;

			slon_mkquery(&query1, "commit transaction;");
			if (query_execute(node, local_dbconn, &query1, PGRES_COMMAND_OK) < 0)
				slon_abort();
		}
		else
		{
			/*
			 * Simple configuration events. Call the corresponding
			 * runtime config function, add the query to call the
			 * configuration event specific stored procedure.
			 */
			if (strcmp(event->ev_type, "STORE_NODE") == 0)
			{
				int		no_id = (int) strtol(event->ev_data1, NULL, 10);
				char   *no_comment = event->ev_data2;

				if (no_id != rtcfg_nodeid)
					rtcfg_storeNode(no_id, no_comment);

				slon_appendquery(&query1,
						"select %s.storeNode_int(%d, '%q'); ",
						rtcfg_namespace,
						no_id, no_comment);
			}
			else if (strcmp(event->ev_type, "ENABLE_NODE") == 0)
			{
				int		no_id = (int) strtol(event->ev_data1, NULL, 10);

				if (no_id != rtcfg_nodeid)
					rtcfg_enableNode(no_id);

				slon_appendquery(&query1,
						"select %s.enableNode_int(%d); ",
						rtcfg_namespace,
						no_id);
			}
			else if (strcmp(event->ev_type, "STORE_PATH") == 0)
			{
				int		pa_server = (int) strtol(event->ev_data1, NULL, 10);
				int		pa_client = (int) strtol(event->ev_data2, NULL, 10);
				char   *pa_conninfo = event->ev_data3;
				int		pa_connretry = (int) strtol(event->ev_data4, NULL, 10);

				if (pa_client == rtcfg_nodeid)
					rtcfg_storePath(pa_server, pa_conninfo, pa_connretry);

				slon_appendquery(&query1,
						"select %s.storePath_int(%d, %d, '%q', %d); ",
						rtcfg_namespace,
						pa_server, pa_client, pa_conninfo, pa_connretry);
			}
			else if (strcmp(event->ev_type, "STORE_LISTEN") == 0)
			{
				int		li_origin = (int) strtol(event->ev_data1, NULL, 10);
				int		li_provider = (int) strtol(event->ev_data2, NULL, 10);
				int		li_receiver = (int) strtol(event->ev_data3, NULL, 10);

				if (li_receiver == rtcfg_nodeid)
					rtcfg_storeListen(li_origin, li_provider);

				slon_appendquery(&query1,
						"select %s.storeListen_int(%d, %d, %d); ",
						rtcfg_namespace,
						li_origin, li_provider, li_receiver);
			}
			else if (strcmp(event->ev_type, "STORE_SET") == 0)
			{
				int		set_id = (int) strtol(event->ev_data1, NULL, 10);
				int		set_origin = (int) strtol(event->ev_data2, NULL, 10);
				char   *set_comment = event->ev_data3;

				if (set_origin != rtcfg_nodeid)
					rtcfg_storeSet(set_id, set_origin, set_comment);

				slon_appendquery(&query1,
						"select %s.storeSet_int(%d, %d, '%q'); ",
						rtcfg_namespace,
						set_id, set_origin, set_comment);
			}
			else if (strcmp(event->ev_type, "SET_ADD_TABLE") == 0)
			{
				/*
				 * Nothing to do ATM ... we don't support
				 * adding tables to subscribed sets yet and
				 * table information is not maintained in
				 * the runtime configuration.
				 */
			}
			else if (strcmp(event->ev_type, "SUBSCRIBE_SET") == 0)
			{
				int		sub_set = (int) strtol(event->ev_data1, NULL, 10);
				int		sub_provider = (int) strtol(event->ev_data2, NULL, 10);
				int		sub_receiver = (int) strtol(event->ev_data3, NULL, 10);
				char   *sub_forward = event->ev_data4;

				if (sub_receiver == rtcfg_nodeid)
					rtcfg_storeSubscribe(sub_set, sub_provider, sub_forward);

				slon_appendquery(&query1,
						"select %s.subscribeSet_int(%d, %d, %d, '%q'); ",
						rtcfg_namespace,
						sub_set, sub_provider, sub_receiver, sub_forward);
			}
			else if (strcmp(event->ev_type, "ENABLE_SUBSCRIPTION") == 0)
			{
				int		sub_set = (int) strtol(event->ev_data1, NULL, 10);
				int		sub_provider = (int) strtol(event->ev_data2, NULL, 10);
				int		sub_receiver = (int) strtol(event->ev_data3, NULL, 10);
				char   *sub_forward  = event->ev_data4;

				/*
				 * Do the actual enabling of the set only if
				 * we are the receiver and if we received this
				 * event from the provider.
				 */
				if (sub_receiver == rtcfg_nodeid &&
					sub_provider == node->no_id)
				{
					SlonDString	query2;
					int			sched_rc;
					int			sleeptime = 15;

					rtcfg_enableSubscription(sub_set, sub_provider, sub_forward);
					dstring_init(&query2);
					slon_mkquery(&query2, "rollback transaction");

					while (true)
					{
						/*
						 * Execute the config changes so far, but don't
						 * commit the transaction yet. We have to copy
						 * the data now ...
						 */
						if (query_execute(node, local_dbconn, &query1,
								PGRES_COMMAND_OK) < 0)
							slon_abort();

						/*
						 * If the copy succeeds, exit the loop and let
						 * the transaction commit.
						 */
						if (copy_set(node, local_conn, sub_set) == 0)
						{
							dstring_free(&query2);
							dstring_reset(&query1);
							sched_rc = SCHED_STATUS_OK;
							break;
						}

						/*
						 * Data copy for new enabled set has failed.
						 * Rollback the transaction, sleep and try again.
						 */
						if (query_execute(node, local_dbconn, &query2,
								PGRES_COMMAND_OK) < 0)
							slon_abort();

						slon_log(SLON_WARN, "remoteWorkerThread_%d: "
								"data copy for set %d failed - "
								"sleep %d seconds\n",
								node->no_id, sub_set, sleeptime);
						sched_rc = sched_msleep(node, sleeptime * 1000);
						if (sched_rc != SCHED_STATUS_OK)
							break;

						if (sleeptime < 60)
							sleeptime *= 2;
					}
				}
			}
			else
			{
printf("TODO: ********** remoteWorkerThread: node %d - EVENT %d,%lld %s - unknown event type\n",
node->no_id, event->ev_origin, event->ev_seqno, event->ev_type);
			}

			/*
			 * All simple configuration events fall through here.
			 * Commit the transaction.
			 */
			slon_appendquery(&query1, 
					"commit transaction;");
			if (query_execute(node, local_dbconn, &query1, PGRES_COMMAND_OK) < 0)
				slon_abort();
		}

		free(msg);
	}

	/*
	 * Thread exit time has arrived.
	 */
	slon_log(SLON_DEBUG1,
			"remoteWorkerThread_%d: thread exiting\n",
			node->no_id);

	/*
	 * Disconnect from all data providers and free memory
	 */
	adjust_provider_info(node, wd, true);

	pthread_mutex_unlock(&(wd->workdata_lock));
	pthread_mutex_destroy(&(wd->workdata_lock));
	pthread_cond_destroy(&(wd->repldata_cond));
	pthread_cond_destroy(&(wd->linepool_cond));

	slon_disconnectdb(local_conn);
	dstring_free(&query1);
	dstring_free(&query2);
	free(wd->tab_fqname);
	free(wd);

	slon_log(SLON_DEBUG1,
			"remoteWorkerThread_%d: thread done\n",
			node->no_id);
	pthread_exit(NULL);
}


/* ----------
 * remoteWorker_event
 *
 *	Used by the remoteListeThread to forward events selected from
 *	the event provider database to the remote nodes worker thread.
 *----------
 */
void
remoteWorker_event(int event_provider,
				int ev_origin, int64 ev_seqno,
				char *ev_timestamp,
				char *ev_minxid, char *ev_maxxid, char *ev_xip,
				char *ev_type, 
				char *ev_data1, char *ev_data2,
				char *ev_data3, char *ev_data4,
				char *ev_data5, char *ev_data6,
				char *ev_data7, char *ev_data8)
{
	SlonNode   *node;
	SlonWorkMsg_event   *msg;
	int			len;
	char	   *cp;
	int			len_timestamp;
	int			len_minxid;
	int			len_maxxid;
	int			len_xip;
	int			len_type;
	int			len_data1 = 0;
	int			len_data2 = 0;
	int			len_data3 = 0;
	int			len_data4 = 0;
	int			len_data5 = 0;
	int			len_data6 = 0;
	int			len_data7 = 0;
	int			len_data8 = 0;

	/*
	 * Stop forwarding events if the replication engine is shutting down
	 */
	if (sched_get_status() != SCHED_STATUS_OK)
	{
		slon_log(SLON_DEBUG2,
				"remoteWorker_event: ignore new events due to shutdown\n");
		return;
	}

	/*
	 * Find the node, make sure it is active and that this event is not
	 * already queued or processed.
	 */
	rtcfg_lock();
	node = rtcfg_findNode(ev_origin);
	if (node == NULL)
	{
		rtcfg_unlock();
		slon_log(SLON_WARN,
				"remoteWorker_event: event %d,%lld ignored - unknown origin\n",
				ev_origin, ev_seqno);
		return;
	}
	if (!node->no_active)
	{
		rtcfg_unlock();
		slon_log(SLON_WARN,
				"remoteWorker_event: event %d,%lld ignored - origin inactive\n",
				ev_origin, ev_seqno);
		return;
	}
	if (node->last_event >= ev_seqno)
	{
		rtcfg_unlock();
		slon_log(SLON_DEBUG2,
				"remoteWorker_event: event %d,%lld ignored - duplicate\n",
				ev_origin, ev_seqno);
		return;
	}

	/*
	 * We lock the worker threads message queue before bumping the
	 * nodes last known event sequence to avoid that another listener
	 * queues a later message before we can insert this one.
	 */
	pthread_mutex_lock(&(node->message_lock));
	node->last_event = ev_seqno;
	rtcfg_unlock();

	/*
	 * Compute the message length and allocate memory. The allocated
	 * memory only needs to be zero-initialized in the structure size.
	 * The following additional space for the event payload data is
	 * overwritten completely anyway.
	 */
	len = offsetof(SlonWorkMsg_event, raw_data)
			+ (len_timestamp = strlen(ev_timestamp) + 1)
			+ (len_minxid = strlen(ev_minxid) + 1)
			+ (len_maxxid = strlen(ev_maxxid) + 1)
			+ (len_xip = strlen(ev_xip) + 1)
			+ (len_type = strlen(ev_type) + 1)
			+ ((ev_data1 == NULL) ? 0 : (len_data1 = strlen(ev_data1) + 1))
			+ ((ev_data2 == NULL) ? 0 : (len_data2 = strlen(ev_data2) + 1))
			+ ((ev_data3 == NULL) ? 0 : (len_data3 = strlen(ev_data3) + 1))
			+ ((ev_data4 == NULL) ? 0 : (len_data4 = strlen(ev_data4) + 1))
			+ ((ev_data5 == NULL) ? 0 : (len_data5 = strlen(ev_data5) + 1))
			+ ((ev_data6 == NULL) ? 0 : (len_data6 = strlen(ev_data6) + 1))
			+ ((ev_data7 == NULL) ? 0 : (len_data7 = strlen(ev_data7) + 1))
			+ ((ev_data8 == NULL) ? 0 : (len_data8 = strlen(ev_data8) + 1));
	msg = (SlonWorkMsg_event *)malloc(len);
	if (msg == NULL)
	{
		perror("remoteWorker_event: malloc()");
		slon_abort();
	}
	memset(msg, 0, sizeof(SlonWorkMsg_event));

	/*
	 * Copy all data into the message.
	 */
	cp = &(msg->raw_data[0]);
	msg->msg_type		= WMSG_EVENT;
	msg->event_provider	= event_provider;
	msg->ev_origin		= ev_origin;
	msg->ev_seqno		= ev_seqno;
	msg->ev_timestamp_c = cp; strcpy(cp, ev_timestamp); cp += len_timestamp;
	msg->ev_minxid_c = cp; strcpy(cp, ev_minxid); cp += len_minxid;
	msg->ev_maxxid_c = cp; strcpy(cp, ev_maxxid); cp += len_maxxid;
	msg->ev_xip = cp; strcpy(cp, ev_xip); cp += len_xip;
	msg->ev_type = cp; strcpy(cp, ev_type); cp += len_type;
	if (ev_data1 != NULL)
	{
		msg->ev_data1 = cp; strcpy(cp, ev_data1); cp += len_data1;
	}
	if (ev_data2 != NULL)
	{
		msg->ev_data2 = cp; strcpy(cp, ev_data2); cp += len_data2;
	}
	if (ev_data3 != NULL)
	{
		msg->ev_data3 = cp; strcpy(cp, ev_data3); cp += len_data3;
	}
	if (ev_data4 != NULL)
	{
		msg->ev_data4 = cp; strcpy(cp, ev_data4); cp += len_data4;
	}
	if (ev_data5 != NULL)
	{
		msg->ev_data5 = cp; strcpy(cp, ev_data5); cp += len_data5;
	}
	if (ev_data6 != NULL)
	{
		msg->ev_data6 = cp; strcpy(cp, ev_data6); cp += len_data6;
	}
	if (ev_data7 != NULL)
	{
		msg->ev_data7 = cp; strcpy(cp, ev_data7); cp += len_data7;
	}
	if (ev_data8 != NULL)
	{
		msg->ev_data8 = cp; strcpy(cp, ev_data8); cp += len_data8;
	}

	/*
	 * Add the message to the queue and trigger the condition
	 * variable in case the worker is idle.
	 */
	DLLIST_ADD_TAIL(node->message_head, node->message_tail,
			(SlonWorkMsg *)msg);
	pthread_cond_signal(&(node->message_cond));
	pthread_mutex_unlock(&(node->message_lock));
}


/* ----------
 * remoteWorker_wakeup
 *
 *	Send a special WAKEUP message to a worker, causing it to recheck
 *	the runmode and the runtime configuration.
 * ----------
 */
void
remoteWorker_wakeup(int no_id)
{
	SlonNode   *node;
	SlonWorkMsg *msg;

	/*
	 * Can't wakeup myself, can I? No, we never have a 
	 * "remote" worker for our own node ID. 
	 */
	if (no_id == rtcfg_nodeid)
		return;

	rtcfg_lock();
	node = rtcfg_findNode(no_id);
	if (node == NULL)
	{
		rtcfg_unlock();
		slon_log(SLON_DEBUG1,
				"remoteWorker_wakeup: unknown node %d\n",
				no_id);
		return;
	}
	if (node->worker_status == SLON_TSTAT_NONE)
	{
		rtcfg_unlock();
		slon_log(SLON_WARN,
				"remoteWorker_wakeup: node %d - no worker thread\n", 
				no_id);
		return;
	}
	rtcfg_unlock();

	msg = (SlonWorkMsg *)malloc(sizeof(SlonWorkMsg));
	msg->msg_type = WMSG_WAKEUP;

	pthread_mutex_lock(&(node->message_lock));
	DLLIST_ADD_TAIL(node->message_head, node->message_tail, msg);
	pthread_cond_signal(&(node->message_cond));
	pthread_mutex_unlock(&(node->message_lock));
}


/* ----------
 * remoteWorker_confirm
 *
 *	Add a confirm message to the remote worker message queue
 * ----------
 */
void
remoteWorker_confirm(int no_id,
		char *con_origin_c, char *con_received_c,
		char *con_seqno_c, char *con_timestamp_c)
{
	SlonNode   *node;
	SlonWorkMsg_confirm *msg;
	SlonWorkMsg_confirm *oldmsg;
	int			con_origin;
	int			con_received;
	int64		con_seqno;

	con_origin = strtol(con_origin_c, NULL, 10);
	con_received = strtol(con_received_c, NULL, 10);
	sscanf(con_seqno_c, "%lld", &con_seqno);

	/*
	 * Check that the node exists and that we have a worker thread.
	 */
	rtcfg_lock();
	node = rtcfg_findNode(no_id);
	if (node == NULL)
	{
		rtcfg_unlock();
		slon_log(SLON_ERROR,
				"remoteWorker_confirm: unknown node %d\n",
				no_id);
		return;
	}
	if (node->worker_status == SLON_TSTAT_NONE)
	{
		rtcfg_unlock();
		slon_log(SLON_WARN,
				"remoteWorker_wakeup: node %d - no worker thread\n", 
				no_id);
		return;
	}
	rtcfg_unlock();


	/*
	 * Lock the message queue
	 */
	pthread_mutex_lock(&(node->message_lock));

	/*
	 * Look if we already have a confirm message for this origin+received
	 * node pair.
	 */
	for (oldmsg = (SlonWorkMsg_confirm *)(node->message_head);
			oldmsg; oldmsg = (SlonWorkMsg_confirm *)(oldmsg->next))
	{
		if (oldmsg->msg_type == WMSG_CONFIRM) {
			if (oldmsg->con_origin == con_origin &&
				oldmsg->con_received == con_received)
			{
				/*
				 * Existing message found. Change it if new seqno is
				 * greater than old. Otherwise just ignore this confirm.
				 */
				if (oldmsg->con_seqno < con_seqno)
				{
					oldmsg->con_seqno = con_seqno;
					strcpy(oldmsg->con_timestamp_c, con_timestamp_c);
				}
				pthread_mutex_unlock(&(node->message_lock));
				return;
			}
		}
	}

	/*
	 * No message found. Create a new one and add it to the queue.
	 */
	msg = (SlonWorkMsg_confirm *)
			malloc(sizeof(SlonWorkMsg_confirm));
	msg->msg_type = WMSG_CONFIRM;

	msg->con_origin = con_origin;
	msg->con_received = con_received;
	msg->con_seqno = con_seqno;
	strcpy(msg->con_timestamp_c, con_timestamp_c);

	DLLIST_ADD_TAIL(node->message_head, node->message_tail, 
			(SlonWorkMsg *)msg);

	/*
	 * Send a condition signal to the worker thread in case it is
	 * waiting for new messages.
	 */
	pthread_cond_signal(&(node->message_cond));
	pthread_mutex_unlock(&(node->message_lock));
}


/* ----------
 * query_execute
 *
 *	Execute a query string that does not return a result set.
 * ----------
 */
static int
query_execute(SlonNode *node, PGconn *dbconn, 
		SlonDString *dsp, int expected_rc)
{
	PGresult   *res;

	res = PQexec(dbconn, dstring_data(dsp));
	if (PQresultStatus(res) != expected_rc)
	{
		slon_log(SLON_ERROR,
				"remoteWorkerThread_%d: \"%s\" %s",
				node->no_id, dstring_data(dsp),
				PQresultErrorMessage(res));
		PQclear(res);
		return -1;
	}
	PQclear(res);
	return 0;
}


/* ----------
 * query_append_event
 *
 *	Add queries to a dstring that notify for Event and Confirm and
 *	that insert a duplicate of an event record as well as the
 *	confirmation for it.
 * ----------
 */
static void
query_append_event(SlonDString *dsp, SlonWorkMsg_event *event)
{
	char		seqbuf[64];
	sprintf(seqbuf, "%lld", event->ev_seqno);

	slon_appendquery(dsp,
			"notify \"_%s_Event\"; "
			"notify \"_%s_Confirm\"; "
			"insert into %s.sl_event "
			"    (ev_origin, ev_seqno, ev_timestamp, "
			"     ev_minxid, ev_maxxid, ev_xip, ev_type ",
			rtcfg_cluster_name, rtcfg_cluster_name,
			rtcfg_namespace);
	if (event->ev_data1 != NULL)	dstring_append(dsp, ", ev_data1");
	if (event->ev_data2 != NULL)	dstring_append(dsp, ", ev_data2");
	if (event->ev_data3 != NULL)	dstring_append(dsp, ", ev_data3");
	if (event->ev_data4 != NULL)	dstring_append(dsp, ", ev_data4");
	if (event->ev_data5 != NULL)	dstring_append(dsp, ", ev_data5");
	if (event->ev_data6 != NULL)	dstring_append(dsp, ", ev_data6");
	if (event->ev_data7 != NULL)	dstring_append(dsp, ", ev_data7");
	if (event->ev_data8 != NULL)	dstring_append(dsp, ", ev_data8");
	slon_appendquery(dsp,
			"    ) values ('%d', '%s', '%s', '%s', '%s', '%q', '%s'",
			event->ev_origin, seqbuf, event->ev_timestamp_c,
			event->ev_minxid_c, event->ev_maxxid_c, event->ev_xip,
			event->ev_type);
	if (event->ev_data1 != NULL)
		slon_appendquery(dsp, ", '%q'", event->ev_data1);
	if (event->ev_data2 != NULL)
		slon_appendquery(dsp, ", '%q'", event->ev_data2);
	if (event->ev_data3 != NULL)
		slon_appendquery(dsp, ", '%q'", event->ev_data3);
	if (event->ev_data4 != NULL)
		slon_appendquery(dsp, ", '%q'", event->ev_data4);
	if (event->ev_data5 != NULL)
		slon_appendquery(dsp, ", '%q'", event->ev_data5);
	if (event->ev_data6 != NULL)
		slon_appendquery(dsp, ", '%q'", event->ev_data6);
	if (event->ev_data7 != NULL)
		slon_appendquery(dsp, ", '%q'", event->ev_data7);
	if (event->ev_data8 != NULL)
		slon_appendquery(dsp, ", '%q'", event->ev_data8);
	slon_appendquery(dsp,
			"); "
			"insert into %s.sl_confirm "
			"	(con_origin, con_received, con_seqno, con_timestamp) "
			"   values (%d, %d, '%s', CURRENT_TIMESTAMP); ",
			rtcfg_namespace,
			event->ev_origin, rtcfg_nodeid, seqbuf);
}


/* ----------
 * store_confirm_forward
 *
 *	Call the forwardConfirm() stored procedure.
 * ----------
 */
static void
store_confirm_forward(SlonNode *node, SlonConn *conn,
		SlonWorkMsg_confirm *confirm)
{
	SlonDString		query;
	PGresult	   *res;
	char			seqbuf[64];
	struct node_confirm_status *cstat;
	int				cstat_found = false;

	/*
	 * Check the global confirm status if we already know about
	 * this confirmation.
	 */
	pthread_mutex_lock(&node_confirm_lock);
	for (cstat = node_confirm_head; cstat; cstat = cstat->next)
	{
		if (cstat->con_origin == confirm->con_origin &&
			cstat->con_received == confirm->con_received)
		{
			/*
			 * origin+received pair record found.
			 */
			if (cstat->con_seqno >= confirm->con_seqno)
			{
				/*
				 * Confirm status is newer or equal, ignore message.
				 */
				pthread_mutex_unlock(&node_confirm_lock);
				return;
			}
			/*
			 * Set the confirm status to the new seqno and continue
			 * below.
			 */
			cstat_found = true;
			cstat->con_seqno = confirm->con_seqno;
			break;
		}
	}

	/*
	 * If there was no such confirm status entry, add a new one.
	 */
	if (!cstat_found)
	{
		cstat = (struct node_confirm_status *)
				malloc(sizeof (struct node_confirm_status));
		cstat->con_origin = confirm->con_origin;
		cstat->con_received = confirm->con_received;
		cstat->con_seqno = confirm->con_seqno;
		DLLIST_ADD_TAIL(node_confirm_head, node_confirm_tail, cstat);
	}

	pthread_mutex_unlock(&node_confirm_lock);

	/*
	 * Call the stored procedure to forward this status through
	 * the table sl_confirm.
	 */
	dstring_init(&query);
	sprintf(seqbuf, "%lld", confirm->con_seqno);
	
	slon_log(SLON_DEBUG2,
			"remoteWorkerThread_%d: forward confirm %d,%s received by %d\n",
			node->no_id, confirm->con_origin, seqbuf, confirm->con_received);

	slon_mkquery(&query,
			"select %s.forwardConfirm(%d, %d, '%s', '%q'); ",
			rtcfg_namespace,
			confirm->con_origin, confirm->con_received,
			seqbuf,  confirm->con_timestamp_c);

	res = PQexec(conn->dbconn, dstring_data(&query));
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_ERROR,
				"remoteWorkerThread_%d: \"%s\" %s",
				node->no_id, dstring_data(&query),
				PQresultErrorMessage(res));
		PQclear(res);
		dstring_free(&query);
		return;
	}
	PQclear(res);
	dstring_free(&query);
	return;
}


static int
copy_set(SlonNode *node, SlonConn *local_conn, int set_id)
{
	SlonConn	   *pro_conn;
	PGconn		   *pro_dbconn;
	PGconn		   *loc_dbconn;
	char		   *conninfo;
	char			conn_symname[64];
	SlonDString		query1;
	SlonDString		query2;
	int				ntuples1;
	int				tupno1;
	PGresult	   *res1;
	PGresult	   *res2;
	PGresult	   *res3;
	int				rc;
	char		   *copydata = NULL;
	int				set_origin;
	char		   *ssy_seqno = NULL;
	char		   *ssy_minxid = NULL;
	char		   *ssy_maxxid = NULL;
	char		   *ssy_xip = NULL;
	SlonDString		ssy_action_list;

	slon_log(SLON_DEBUG1, "******* copy_set %d\n", set_id);

	/*
	 * Connect to the provider DB
	 */
	rtcfg_lock();
	conninfo = strdup(node->pa_conninfo);
	rtcfg_unlock();
	sprintf(conn_symname, "copy_set_%d", set_id);
	if ((pro_conn = slon_connectdb(conninfo, conn_symname)) == NULL)
	{
		free(conninfo);
		slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
				"copy set %d - cannot connect to provider DB\n",
				node->no_id, set_id);
		return -1;
	}
	free(conninfo);
	slon_log(SLON_DEBUG2, "remoteWorkerThread_%d: "
			"connected to provider DB\n",
			node->no_id);

	pro_dbconn = pro_conn->dbconn;
	loc_dbconn = local_conn->dbconn;
	dstring_init(&query1);

	/*
	 * Begin a serialized transaction and select the list of all
	 * tables the provider currently has in the set.
	 */
	slon_mkquery(&query1,
			"start transaction; "
			"set transaction isolation level serializable; "
			"select T.tab_id, "
			"    \"pg_catalog\".quote_ident(PGN.nspname) || '.' || "
			"    \"pg_catalog\".quote_ident(PGC.relname) as tab_fqname, "
			"    T.tab_attkind, T.tab_comment "
			"from %s.sl_table T, "
			"    \"pg_catalog\".pg_class PGC, "
			"    \"pg_catalog\".pg_namespace PGN "
			"where T.tab_set = %d "
			"    and T.tab_reloid = PGC.oid "
			"    and PGC.relnamespace = PGN.oid "
			"order by tab_id; ",
			rtcfg_namespace, set_id);
	res1 = PQexec(pro_dbconn, dstring_data(&query1));
	if (PQresultStatus(res1) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_ERROR, "remoteWorkerThread_%d: \"%s\" %s",
				node->no_id, dstring_data(&query1),
				PQresultErrorMessage(res1));
		PQclear(res1);
		slon_disconnectdb(pro_conn);
		dstring_free(&query1);
		return -1;
	}
	ntuples1 = PQntuples(res1);

	/*
	 * For each table in the set
	 */
	for (tupno1 = 0; tupno1 < ntuples1; tupno1++)
	{
		int		tab_id		= strtol(PQgetvalue(res1, tupno1, 0), NULL, 10);
		char   *tab_fqname	= PQgetvalue(res1, tupno1, 1);
		char   *tab_attkind	= PQgetvalue(res1, tupno1, 2);
		char   *tab_comment	= PQgetvalue(res1, tupno1, 3);
		int64	copysize	= 0;

		slon_log(SLON_DEBUG2, "remoteWorkerThread_%d: "
				"tab_id=%d tab_fqname=%s tab_attkind=%s tab_comment='%s'\n",
				node->no_id,
				tab_id, tab_fqname, tab_attkind, tab_comment);

		/*
		 * Call the setAddTable_int() stored procedure. Up to now, while
		 * we have not been subscribed to the set, this should have been
		 * suppressed.
		 */
		slon_mkquery(&query1,
				"select %s.setAddTable_int(%d, %d, '%q', '%q', '%q'); ",
				rtcfg_namespace,
				set_id, tab_id, tab_fqname, tab_attkind, tab_comment);
		if (query_execute(node, loc_dbconn, &query1, PGRES_TUPLES_OK) < 0)
		{
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}

		/*
		 * Begin a COPY from stdin for the table on the local DB
		 */
		slon_mkquery(&query1, "copy %s from stdin; ", tab_fqname);
		res2 = PQexec(loc_dbconn, dstring_data(&query1));
		if (PQresultStatus(res2) != PGRES_COPY_IN)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: \"%s\" %s %s\n",
					node->no_id, dstring_data(&query1),
					PQresultErrorMessage(res2),
					PQerrorMessage(loc_dbconn));
			PQclear(res2);
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}

		/*
		 * Begin a COPY to stdout for the table on the provider DB
		 */
		slon_mkquery(&query1, "copy %s to stdout; ", tab_fqname);
		res3 = PQexec(pro_dbconn, dstring_data(&query1));
		if (PQresultStatus(res3) != PGRES_COPY_OUT)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: \"%s\" %s %s\n",
					node->no_id, dstring_data(&query1),
					PQresultErrorMessage(res2),
					PQerrorMessage(pro_dbconn));
			PQputCopyEnd(loc_dbconn, "Slony-I: copy set operation failed");
			PQclear(res3);
			PQclear(res2);
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}

		/*
		 * Copy the data over
		 */
		while ((rc = PQgetCopyData(pro_dbconn, &copydata, 0)) > 0)
		{
			int		len = strlen(copydata);

			copysize += (int64)len;

			if (PQputCopyData(loc_dbconn, copydata, len) != 1)
			{
				slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
						"PQputCopyData() - %s",
						node->no_id, PQerrorMessage(loc_dbconn));
				PQfreemem(copydata);
				PQputCopyEnd(loc_dbconn, "Slony-I: copy set operation failed");
				PQclear(res3);
				PQclear(res2);
				PQclear(res1);
				slon_disconnectdb(pro_conn);
				dstring_free(&query1);
				return -1;
			}
			PQfreemem(copydata);
		}
		if (rc != -1)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
					"PGgetCopyData() %s",
					node->no_id, PQerrorMessage(pro_dbconn)); 
			PQputCopyEnd(loc_dbconn, "Slony-I: copy set operation failed");
			PQclear(res3);
			PQclear(res2);
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}

		/*
		 * Check that the COPY to stdout on the provider node
		 * finished successful.
		 */
		res3 = PQgetResult(pro_dbconn);
		if (PQresultStatus(res3) != PGRES_COMMAND_OK)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
					"copy to stdout on provider - %s %s",
					node->no_id, PQresStatus(PQresultStatus(res3)),
					PQresultErrorMessage(res3));
			PQputCopyEnd(loc_dbconn, "Slony-I: copy set operation failed");
			PQclear(res3);
			PQclear(res2);
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}
		PQclear(res3);

		/*
		 * End the COPY from stdin on the local node with success
		 */
		if (PQputCopyEnd(loc_dbconn, NULL) != 1)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
					"PGputCopyEnd() %s",
					node->no_id, PQerrorMessage(loc_dbconn)); 
			PQclear(res2);
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}
		res2 = PQgetResult(loc_dbconn);
		if (PQresultStatus(res2) != PGRES_COMMAND_OK)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
					"copy from stdin on local node - %s %s",
					node->no_id, PQresStatus(PQresultStatus(res2)),
					PQresultErrorMessage(res2));
			PQclear(res2);
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}
		PQclear(res2);

		slon_log(SLON_DEBUG2, "remoteWorkerThread_%d: "
				"%lld bytes copied for table %s\n",
				node->no_id, copysize, tab_fqname);
	}
	PQclear(res1);

	/*
	 * Determine the set origin
	 */
	slon_mkquery(&query1,
			"select set_origin from %s.sl_set where set_id = %d; ",
			rtcfg_namespace, set_id);
	res1 = PQexec(pro_dbconn, dstring_data(&query1));
	if (PQresultStatus(res1) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_ERROR, "remoteWorkerThread_%d: \"%s\" %s",
				node->no_id, dstring_data(&query1),
				PQresultErrorMessage(res1));
		PQclear(res1);
		slon_disconnectdb(pro_conn);
		dstring_free(&query1);
		return -1;
	}
	if (PQntuples(res1) != 1)
	{
		slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
				"cannot determine origin of set %d\n",
				node->no_id, set_id);
		PQclear(res1);
		slon_disconnectdb(pro_conn);
		dstring_free(&query1);
		return -1;
	}
	set_origin = strtol(PQgetvalue(res1, 0, 0), NULL, 10);
	PQclear(res1);

	/*
	 * It depends on who is our data provider how we construct
	 * the initial setsync status.
	 */
	if (set_origin == node->no_id)
	{
		/*
		 * Our provider is the origin, so we have to construct
		 * the setsync from scratch. Let's see if there is any
		 * SYNC event known.
		 */
		slon_mkquery(&query1,
				"select max(ev_seqno) as ssy_seqno "
				"from %s.sl_event "
				"where ev_origin = %d and ev_type = 'SYNC'; ",
				rtcfg_namespace, node->no_id);
		res1 = PQexec(pro_dbconn, dstring_data(&query1));
		if (PQresultStatus(res1) != PGRES_TUPLES_OK)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: \"%s\" %s",
					node->no_id, dstring_data(&query1),
					PQresultErrorMessage(res1));
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}
		if (PQntuples(res1) != 1)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
					"query \"%s\" did not return a result\n",
					node->no_id, dstring_data(&query1));
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}
		if (PQgetisnull(res1, 0, 0))
		{
			/*
			 * No SYNC event found, so we initialize the setsync to
			 * zeroes with ALL action sequences that exist.
			 */
			ssy_seqno	= "0";
			ssy_minxid	= "0";
			ssy_maxxid	= "0";
			ssy_xip		= "";

			slon_mkquery(&query1,
					"select log_actionseq "
					"from %s.sl_log_1 where log_origin = %d "
					"union select log_actionseq "
					"from %s.sl_log_2 where log_origin = %d; ",
					rtcfg_namespace, node->no_id,
					rtcfg_namespace, node->no_id);
		}
		else
		{
			/*
			 * Use the last SYNC's snapshot information and
			 * set the action sequence list to all actions after that.
			 */
			slon_mkquery(&query1,
					"select ev_seqno, ev_minxid, ev_maxxid, ev_xip "
					"from %s.sl_event where ev_seqno = '%s'; ",
					rtcfg_namespace, PQgetvalue(res1, 0, 0));
			PQclear(res1);
			res1 = PQexec(pro_dbconn, dstring_data(&query1));
			if (PQresultStatus(res1) != PGRES_TUPLES_OK)
			{
				slon_log(SLON_ERROR, "remoteWorkerThread_%d: \"%s\" %s",
						node->no_id, dstring_data(&query1),
						PQresultErrorMessage(res1));
				PQclear(res1);
				slon_disconnectdb(pro_conn);
				dstring_free(&query1);
				return -1;
			}
			if (PQntuples(res1) != 1)
			{
				slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
						"query \"%s\" did not return a result\n",
						node->no_id, dstring_data(&query1));
				PQclear(res1);
				slon_disconnectdb(pro_conn);
				dstring_free(&query1);
				return -1;
			}

			ssy_seqno	= PQgetvalue(res1, 0, 0);
			ssy_minxid	= PQgetvalue(res1, 0, 1);
			ssy_maxxid	= PQgetvalue(res1, 0, 2);
			ssy_xip		= PQgetvalue(res1, 0, 3);

			dstring_init(&query2);
			slon_mkquery(&query2,
					"log_xid > '%s' or (log_xid >= '%s'",
					ssy_maxxid, ssy_minxid);
			if (strlen(ssy_xip) != 0)
				slon_appendquery(&query2, " and log_xid in (%s))", ssy_xip);
			else
				slon_appendquery(&query2, ")");
			
			slon_mkquery(&query1,
					"select log_actionseq "
					"from %s.sl_log_1 where log_origin = %d and %s "
					"union select log_actionseq "
					"from %s.sl_log_2 where log_origin = %d and %s; ",
					rtcfg_namespace, node->no_id, dstring_data(&query2),
					rtcfg_namespace, node->no_id, dstring_data(&query2));
			dstring_free(&query2);
		}

		/*
		 * query1 now contains the selection for the ssy_action_list
		 * selection from both log tables. Fill the dstring.
		 */
		res2 = PQexec(pro_dbconn, dstring_data(&query1));
		if (PQresultStatus(res2) != PGRES_TUPLES_OK)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: \"%s\" %s",
					node->no_id, dstring_data(&query1),
					PQresultErrorMessage(res2));
			PQclear(res2);
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}
		ntuples1 = PQntuples(res2);
		dstring_init(&ssy_action_list);
		if (ntuples1 > 0)
		{
			dstring_addchar(&ssy_action_list, '\'');
			dstring_append(&ssy_action_list, PQgetvalue(res2, 0, 0));
			dstring_addchar(&ssy_action_list, '\'');
		}
		for (tupno1 = 1; tupno1 < ntuples1; tupno1++)
		{
			dstring_addchar(&ssy_action_list, ',');
			dstring_addchar(&ssy_action_list, '\'');
			dstring_append(&ssy_action_list, PQgetvalue(res2, tupno1, 0));
			dstring_addchar(&ssy_action_list, '\'');
		}
		dstring_terminate(&ssy_action_list);
		PQclear(res2);
	}
	else
	{
		/*
		 * Our provider is another slave, so we can copy the
		 * existing setsync from him.
		 */
		slon_mkquery(&query1,
				"select ssy_seqno, ssy_minxid, ssy_maxxid, "
				"    ssy_xip, ssy_action_list "
				"from %s.sl_setsync where ssy_setid = %d; ",
				rtcfg_namespace, set_id);
		res1 = PQexec(pro_dbconn, dstring_data(&query1));
		if (PQresultStatus(res1) != PGRES_TUPLES_OK)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: \"%s\" %s",
					node->no_id, dstring_data(&query1),
					PQresultErrorMessage(res1));
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}
		if (PQntuples(res1) != 1)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
					"sl_setsync entry for set %d not found on provider\n",
					node->no_id, set_id);
			PQclear(res1);
			slon_disconnectdb(pro_conn);
			dstring_free(&query1);
			return -1;
		}

		dstring_init(&ssy_action_list);
		ssy_seqno	= PQgetvalue(res1, 0, 0);
		ssy_minxid	= PQgetvalue(res1, 0, 1);
		ssy_maxxid	= PQgetvalue(res1, 0, 2);
		ssy_xip		= PQgetvalue(res1, 0, 3);
		dstring_append(&ssy_action_list, PQgetvalue(res1, 0, 4));
		dstring_terminate(&ssy_action_list);
	}

	/*
	 * Create our own initial setsync entry
	 */
	slon_mkquery(&query1,
			"insert into %s.sl_setsync "
			"    (ssy_setid, ssy_origin, ssy_seqno, "
			"     ssy_minxid, ssy_maxxid, ssy_xip, ssy_action_list) "
			"    values ('%d', '%d', '%s', '%s', '%s', '%q', '%q'); ",
			rtcfg_namespace,
			set_id, node->no_id, ssy_seqno, ssy_minxid, ssy_maxxid, ssy_xip,
			dstring_data(&ssy_action_list));
	PQclear(res1);
	dstring_free(&ssy_action_list);
	if (query_execute(node, loc_dbconn, &query1, PGRES_COMMAND_OK) < 0)
	{
		slon_disconnectdb(pro_conn);
		dstring_free(&query1);
		return -1;
	}

	/*
	 * Roll back the transaction we used on the provider and close
	 * the database connection.
	 */
	slon_mkquery(&query1, "rollback transaction");
	if (query_execute(node, pro_dbconn, &query1, PGRES_COMMAND_OK) < 0)
	{
		slon_disconnectdb(pro_conn);
		dstring_free(&query1);
		return -1;
	}
	slon_disconnectdb(pro_conn);
	dstring_free(&query1);

	return 0;
}


static int
sync_event(SlonNode *node, SlonConn *local_conn, 
		WorkerGroupData *wd, SlonWorkMsg_event *event)
{
	ProviderInfo   *provider;
	ProviderSet	   *pset;
	char			conn_symname[64];
	/* TODO: tab_forward array to know if we need to store the log */
	PGconn		   *local_dbconn = local_conn->dbconn;
	PGresult	   *res1;
	int				num_providers_active = 0;
	int				num_errors;
	WorkerGroupLine *wgline;
	int				i;
	char			seqbuf[64];

	SlonDString		new_qual;
	SlonDString		query;
	SlonDString	   *provider_qual;

	slon_log(SLON_DEBUG1, "remoteWorkerThread_%d: SYNC %lld processing\n",
			node->no_id, event->ev_seqno);

	/*
	 * Establish all required data provider connections
	 */
	for (provider = wd->provider_head; provider; provider = provider->next)
	{
		if (provider->conn == NULL)
		{
			if (provider->pa_conninfo == NULL)
			{
				slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
						"No pa_conninfo for data provider %d\n",
						node->no_id, provider->no_id);
				return 10;
			}

			sprintf(conn_symname, "subscriber_%d_provider_%d",
					node->no_id, provider->no_id);
			provider->conn = slon_connectdb(provider->pa_conninfo,
					conn_symname);
			if (provider->conn == NULL)
			{
				slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
						"cannot connect to data provider %d on '%s'\n",
						node->no_id, provider->no_id,
						provider->pa_conninfo);
				return provider->pa_connretry;
			}

			slon_log(SLON_DEBUG1, "remoteWorkerThread_%d: "
					"connected to data provider %d on '%s'\n",
					node->no_id, provider->no_id,
					provider->pa_conninfo);
		}
	}

	/*
	 * Check that all these providers have processed at least up
	 * to the SYNC event we're handling here.
	 */
	for (provider = wd->provider_head; provider; provider = provider->next)
	{
		/*
		 * We only need to explicitly check this if the data provider
		 * is neither the set origin, nor the node we received this
		 * event from.
		 */
		if (event->ev_origin != provider->no_id &&
			event->event_provider != provider->no_id)
		{
			slon_log(SLON_FATAL, "remoteWorkerThread_%d: "
					"need to check sync status of data provider\n",
					node->no_id);
			slon_abort();
		}
	}

	dstring_init(&query);
	dstring_init(&new_qual);


	if (strlen(event->ev_xip) != 0)
		slon_mkquery(&new_qual, 
				"(log_xid < '%s' or (log_xid <= '%s' and log_xid not in (%s)))",
				event->ev_minxid_c, event->ev_maxxid_c, event->ev_xip);
	else
		slon_mkquery(&new_qual, 
				"(log_xid <= '%s')",
				event->ev_maxxid_c);

	for (provider = wd->provider_head; provider; provider = provider->next)
	{
		char	   *where_or_or = "where";
		int			ntuples1;
		int			tupno1;
		PGresult   *res2;
		int			ntuples2;
		int			tupno2;

		provider_qual = &(provider->helper_qualification);
		dstring_reset(provider_qual);

		/*
		 * Select all sets we receive from this provider
		 */
		slon_mkquery(&query,
				"select S.sub_set, SSY.ssy_seqno, "
				"    SSY.ssy_minxid, SSY.ssy_maxxid, SSY.ssy_xip, "
				"    SSY.ssy_action_list "
				"from %s.sl_subscribe S, %s.sl_setsync SSY "
				"where S.sub_provider = %d "
				"    and S.sub_receiver = %d "
				"    and S.sub_set = SSY.ssy_setid; ",
				rtcfg_namespace, rtcfg_namespace,
				provider->no_id, rtcfg_nodeid);
		res1 = PQexec(local_dbconn, dstring_data(&query));
		if (PQresultStatus(res1) != PGRES_TUPLES_OK)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: \"%s\" %s",
					node->no_id, dstring_data(&query),
					PQresultErrorMessage(res1));
			PQclear(res1);
			dstring_free(&new_qual);
			dstring_free(&query);
			return 60;
		}


		/* 
		 * For every set we receive from this provider
		 */
		ntuples1 = PQntuples(res1);
		for (tupno1 = 0; tupno1 < ntuples1; tupno1++)
		{
			int		sub_set		= strtol(PQgetvalue(res1, tupno1, 0), NULL, 10);
			char   *ssy_seqno	= PQgetvalue(res1, tupno1, 1);
			char   *ssy_minxid	= PQgetvalue(res1, tupno1, 2);
			char   *ssy_maxxid	= PQgetvalue(res1, tupno1, 3);
			char   *ssy_xip		= PQgetvalue(res1, tupno1, 4);
			char   *ssy_action_list	= PQgetvalue(res1, tupno1, 5);

			/*
			 * Select the tables in that set ...
			 */
			slon_mkquery(&query, 
					"select T.tab_id, T.tab_set, "
					"    \"pg_catalog\".quote_ident(PGN.nspname) || '.' || "
					"    \"pg_catalog\".quote_ident(PGC.relname) as tab_fqname "
					"from %s.sl_table T, "
					"    \"pg_catalog\".pg_class PGC, "
					"    \"pg_catalog\".pg_namespace PGN "
					"where T.tab_set = %d "
					"    and PGC.oid = T.tab_reloid "
					"    and PGC.relnamespace = PGN.oid; ",
					rtcfg_namespace, sub_set);
			res2 = PQexec(local_dbconn, dstring_data(&query));
			if (PQresultStatus(res2) != PGRES_TUPLES_OK)
			{
				slon_log(SLON_ERROR, "remoteWorkerThread_%d: \"%s\" %s",
						node->no_id, dstring_data(&query),
						PQresultErrorMessage(res2));
				PQclear(res2);
				PQclear(res1);
				dstring_free(&new_qual);
				dstring_free(&query);
				return 60;
			}
			ntuples2 = PQntuples(res2);
			if (ntuples2 == 0)
			{
				PQclear(res2);
				continue;
			}

			/*
			 * ... and build up a query qualification that is
			 * 
			 *  where (log_tableid in (<tables_in_set>) 
			 *         and (<snapshot_qual_of_new_sync>)
			 *         and (<snapshot_qual_of_setsync>)
			 *        )
			 *  and   ( <next_set_from_this_provider> )
			 */
			slon_appendquery(provider_qual, 
					"%s (\n    log_tableid in (", where_or_or);
			where_or_or = "or";

			/* the <tables_in_set> tab_id list */
			for (tupno2 = 0; tupno2 < ntuples2; tupno2++)
			{
				int		tab_id		= strtol(PQgetvalue(res2, tupno2, 0), NULL, 10);

				/*
				 * Remember the full qualified table name on the fly.
				 * This might have to become a hashtable someday.
				 */
				while (tab_id >= wd->tab_fqname_size)
				{
					wd->tab_fqname = (char **)realloc(wd->tab_fqname,
							sizeof(char *) * wd->tab_fqname_size * 2);
					memset (&(wd->tab_fqname[wd->tab_fqname_size]), 0,
							sizeof(char *) * wd->tab_fqname_size);
					wd->tab_fqname_size *= 2;
				}
				wd->tab_fqname[tab_id] = strdup(PQgetvalue(res2, tupno2, 2));

				if (tupno2 > 0)
					dstring_addchar(provider_qual, ',');
				dstring_append(provider_qual, PQgetvalue(res2, tupno2, 0));
			}

			/* add the <snapshot_qual_of_new_sync> */
			slon_appendquery(provider_qual,
					")\n    and %s\n    and ",
					dstring_data(&new_qual));
			
			/* add the <snapshot_qual_of_setsync> */
			if (strlen(ssy_xip) != 0)
				slon_appendquery(provider_qual,
						"(log_xid > '%s' or"
						" (log_xid >= '%s' and log_xid in (%s)))",
						ssy_maxxid, ssy_minxid, ssy_xip);
			else
				slon_appendquery(provider_qual,
						"(log_xid >= '%s')",
						ssy_minxid);
			if (strlen(ssy_action_list) != 0)
				slon_appendquery(provider_qual,
						" and log_actionseq not in (%s)\n) ",
						ssy_action_list);
			else
				slon_appendquery(provider_qual, "\n) ");


			PQclear(res2);
		}
		PQclear(res1);
	}

	dstring_free(&new_qual);

	/*
	 * Time to get the helpers busy.
	 */
	wd->workgroup_status = SLON_WG_BUSY;
	pthread_mutex_unlock(&(wd->workdata_lock));
	for (provider = wd->provider_head; provider; provider = provider->next)
	{
		pthread_mutex_lock(&(provider->helper_lock));
		slon_log(SLON_DEBUG2, "remoteWorkerThread_%d: "
				"activate helper %d\n",
				node->no_id, provider->no_id);
		provider->helper_status = SLON_WG_BUSY;
		pthread_cond_signal(&(provider->helper_cond));
		pthread_mutex_unlock(&(provider->helper_lock));
		num_providers_active++;
	}

	num_errors = 0;
	while (num_providers_active > 0)
	{
		WorkerGroupLine	   *lines_head = NULL;
		WorkerGroupLine	   *lines_tail = NULL;
		WorkerGroupLine	   *wgnext = NULL;

		/*
		 * Consume the replication data from the providers
		 */
		pthread_mutex_lock(&(wd->workdata_lock));
		while (wd->repldata_head == NULL)
		{
			pthread_cond_wait(&(wd->repldata_cond), &(wd->workdata_lock));
		}
		lines_head = wd->repldata_head;
		lines_tail = wd->repldata_tail;
		wd->repldata_head = NULL;
		wd->repldata_tail = NULL;
		pthread_mutex_unlock(&(wd->workdata_lock));

		for (wgline = lines_head; wgline && num_errors == 0; 
				wgline = wgline->next)
		{
			/*
			 * Got a line ... process content
			 */
			switch (wgline->code)
			{
				case SLON_WGLC_ACTION:
					res1 = PQexec(local_dbconn, dstring_data(&(wgline->data)));
					if (PQresultStatus(res1) != PGRES_COMMAND_OK)
					{
						slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
								"\"%s\" %s",
								node->no_id, dstring_data(&(wgline->data)),
								PQresultErrorMessage(res1));
						num_errors++;
					}
					else
					{
						if (strtol(PQcmdTuples(res1), NULL, 10) != 1)
						{
							slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
									"replication query did not affect "
									"one data row (cmdTuples = %s) - "
									"query was: %s",
									node->no_id, PQcmdTuples(res1),
									dstring_data(&(wgline->data)));
							num_errors++;
						}
					}
					break;

				case SLON_WGLC_DONE:
					provider = wgline->provider;
					slon_log(SLON_DEBUG2, "remoteWorkerThread_%d: "
							"helper %d finished\n",
							node->no_id, wgline->provider->no_id);
					num_providers_active--;
					break;

				case SLON_WGLC_ERROR:
					provider = wgline->provider;
					slon_log(SLON_ERROR, "remoteWorkerThread_%d: "
							"helper %d finished with error\n",
							node->no_id, wgline->provider->no_id);
					num_providers_active--;
					num_errors++;
					break;
			}
		}

		/*
		 * Put the line buffers back into the pool.
		 */
		pthread_mutex_lock(&(wd->workdata_lock));
		for (wgline = lines_head; wgline; wgline = wgnext)
		{
			wgnext = wgline->next;
			dstring_reset(&(wgline->data));
			DLLIST_ADD_HEAD(wd->linepool_head, wd->linepool_tail, wgline);
		}
		if (num_errors == 1)
			wd->workgroup_status = SLON_WG_ABORT;
		pthread_cond_broadcast(&(wd->linepool_cond));
		pthread_mutex_unlock(&(wd->workdata_lock));
	}

	pthread_mutex_lock(&(wd->workdata_lock));

	/*
	 * Inform the helpers that the whole group is done with this
	 * SYNC.
	 */
	slon_log(SLON_DEBUG2, "remoteWorkerThread_%d: "
			"all helpers done.\n",
			node->no_id);
	for (provider = wd->provider_head; provider; provider = provider->next)
	{
		pthread_mutex_lock(&(provider->helper_lock));
		provider->helper_status = SLON_WG_IDLE;
		pthread_cond_signal(&(provider->helper_cond));
		pthread_mutex_unlock(&(provider->helper_lock));
	}

	/*
	 * Cleanup
	 */
	for (i = 0; i < wd->tab_fqname_size; i++)
	{
		if (wd->tab_fqname[i] != NULL)
		{
			free(wd->tab_fqname[i]);
			wd->tab_fqname[i] = NULL;
		}
	}

	/*
	 * If there have been any errors, abort the SYNC
	 */
	if (num_errors != 0)
	{
		dstring_free(&query);
		slon_log(SLON_ERROR, "remoteWorkerThread_%d: SYNC aborted\n",
				node->no_id);
		return 10;
	}

	/*
	 * Light's are still green ... update the setsync status of
	 * all the sets we've just replicated ...
	 */
	sprintf(seqbuf, "%lld", event->ev_seqno);
	slon_mkquery(&query,
			"update %s.sl_setsync set ssy_origin = '%d', "
			"    ssy_seqno = '%s', ssy_minxid = '%s', ssy_maxxid = '%s', "
			"    ssy_xip = '%q', ssy_action_list = '' "
			"where ssy_setid in (",
			rtcfg_namespace, event->ev_origin,
			seqbuf, event->ev_minxid_c, event->ev_maxxid_c,
			event->ev_xip);
	i = 0;
	for (provider = wd->provider_head; provider; provider = provider->next)
	{
		for (pset = provider->set_head; pset; pset = pset->next)
		{
			slon_appendquery(&query, "%s%d", (i == 0) ? "" : ",",
					pset->set_id);
			i++;
		}
	}

	if (i > 0)
	{
		/*
 		 * ... if there could be any, that is.
		 */
		slon_appendquery(&query, "); ");
		res1 = PQexec(local_dbconn, dstring_data(&query));
		if (PQresultStatus(res1) != PGRES_COMMAND_OK)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: \"%s\" %s",
					node->no_id, dstring_data(&query),
					PQresultErrorMessage(res1));
			PQclear(res1);
			dstring_free(&query);
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: SYNC aborted\n",
					node->no_id);
			return 10;
		}
		if (strtol(PQcmdTuples(res1), NULL, 10) != i)
		{
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: update of sl_setsync "
					"modified %s rows - expeced %d\n",
					node->no_id, PQcmdTuples(res1), i);
			slon_log(SLON_DEBUG1, "remoteWorkerThread_%d: query was: %s\n",
					node->no_id, dstring_data(&query));
			PQclear(res1);
			dstring_free(&query);
			slon_log(SLON_ERROR, "remoteWorkerThread_%d: SYNC aborted\n",
					node->no_id);
			return 10;
		}
		PQclear(res1);
	}

	/*
	 * Good job!
	 */
	dstring_free(&query);
	slon_log(SLON_DEBUG1, "remoteWorkerThread_%d: SYNC %lld done\n",
			node->no_id, event->ev_seqno);
	return 0;
}


static void *
sync_helper(void *cdata)
{
	ProviderInfo	   *provider = (ProviderInfo *)cdata;
	WorkerGroupData	   *wd = provider->wd;
	SlonNode		   *node = wd->node;
	PGconn			   *dbconn;
	WorkerGroupLine	   *line;
	SlonDString			query;
	PGresult		   *res;
	int					ntuples;
	int					tupno;
	int					errors;
	WorkerGroupLine	   *data_line[SLON_DATA_FETCH_SIZE];
	int					alloc_lines = 0;

	for (;;)
	{
		pthread_mutex_lock(&(provider->helper_lock));
		while (provider->helper_status == SLON_WG_IDLE)
		{
			slon_log(SLON_DEBUG1, "remoteHelperThread_%d_%d: "
					"waiting for work\n",
					node->no_id, provider->no_id);

			pthread_cond_wait(&(provider->helper_cond), &(provider->helper_lock));
		}

		if (provider->helper_status == SLON_WG_EXIT)
		{
			pthread_mutex_unlock(&(provider->helper_lock));
			pthread_exit(NULL);
		}
		if (provider->helper_status != SLON_WG_BUSY)
		{
			provider->helper_status = SLON_WG_IDLE;
			pthread_mutex_unlock(&(provider->helper_lock));
			continue;
		}

		/*
		 * OK, we got work to do.
		 */
		dbconn = provider->conn->dbconn;
		pthread_mutex_unlock(&(provider->helper_lock));

		errors = 0;
		do {
			/*
			 * Start a transaction
			 */
			dstring_init(&query);
			slon_mkquery(&query, "start transaction; ");
			if (query_execute(node, dbconn, &query, PGRES_COMMAND_OK) < 0)
			{
				errors++;
				break;
			}

			/*
			 * Open a cursor that reads the log data.
			 *
			 * TODO: need to change this into a conditional sl_log_n
			 * selection depending on the logstatus.
			 */
			slon_mkquery(&query,
					"declare LOG cursor for select "
					"    log_origin, log_xid, log_tableid, "
					"    log_actionseq, log_cmdtype, log_cmddata "
					"from %s.sl_log_1 %s order by log_actionseq; ",
					rtcfg_namespace, 
					dstring_data(&(provider->helper_qualification)));
			if (query_execute(node, dbconn, &query, PGRES_COMMAND_OK) < 0)
			{
				errors++;
				break;
			}

			/*
			 * Now fetch the log data and forward it via the line
			 * pool to the main worker who pushes it into the local
			 * database.
			 */
			alloc_lines = 0;
			while (errors == 0)
			{
				/*
				 * Allocate at least some lines - ideally the whole
				 * fetch size.
				 */
				while (alloc_lines == 0 && !errors)
				{
					/*
					 * Wait until there are lines available in
					 * the pool.
					 */
					pthread_mutex_lock(&(wd->workdata_lock));
					while (wd->linepool_head == NULL &&
							wd->workgroup_status == SLON_WG_BUSY)
					{
						pthread_cond_wait(&(wd->linepool_cond), &(wd->workdata_lock));
					}

					/*
					 * If any error occured somewhere in the group, the
					 * main worker will set the status to ABORT.
					 */
					if (wd->workgroup_status != SLON_WG_BUSY)
					{
						pthread_mutex_unlock(&(wd->workdata_lock));
						errors++;
						break;
					}

					/*
					 * So far so good. Fill our array of lines
					 * from the pool.
					 */
					while (alloc_lines < SLON_DATA_FETCH_SIZE &&
							wd->linepool_head != NULL)
					{
						data_line[alloc_lines] = wd->linepool_head;
						DLLIST_REMOVE(wd->linepool_head, wd->linepool_tail,
								data_line[alloc_lines]);
						alloc_lines++;
					}
					pthread_mutex_unlock(&(wd->workdata_lock));
				}

				if (errors)
					break;

				/*
				 * Now that we have allocated some buffer space,
				 * try to fetch that many rows from the cursor.
				 */
				slon_mkquery(&query, "fetch %d from LOG; ", alloc_lines);
				res = PQexec(dbconn, dstring_data(&query));
				if (PQresultStatus(res) != PGRES_TUPLES_OK)
				{
					slon_log(SLON_DEBUG1, "remoteHelperThread_%d_%d: \"%s\" %s",
							node->no_id, provider->no_id,
							dstring_data(&query),
							PQresultErrorMessage(res));
					errors++;
					break;
				}

				/*
				 * Fill the line buffers with queries from the
				 * retrieved log rows.
				 */
				ntuples = PQntuples(res);
				for (tupno = 0; tupno < ntuples; tupno++)
				{
					char   *log_origin		= PQgetvalue(res, tupno, 0);
					char   *log_xid			= PQgetvalue(res, tupno, 1);
					int		log_tableid		= strtol(PQgetvalue(res, tupno, 2),
																NULL, 10);
					char   *log_actionseq	= PQgetvalue(res, tupno, 3);
					char   *log_cmdtype		= PQgetvalue(res, tupno, 4);
					char   *log_cmddata		= PQgetvalue(res, tupno, 5);

					line = data_line[tupno];
					line->code = SLON_WGLC_ACTION;
					line->provider = provider;
					switch (*log_cmdtype)
					{
						case 'I':
							slon_mkquery(&(line->data),
									"-- log_xid %s\n"
									"-- log_actionseq %s\n"
									"insert into %s %s;",
									log_xid, log_actionseq,
									wd->tab_fqname[log_tableid], 
									log_cmddata);
							break;

						case 'U':
							slon_mkquery(&(line->data),
									"-- log_xid %s\n"
									"-- log_actionseq %s\n"
									"update %s set %s;",
									log_xid, log_actionseq,
									wd->tab_fqname[log_tableid], 
									log_cmddata);
							break;

						case 'D':
							slon_mkquery(&(line->data),
									"-- log_xid %s\n"
									"-- log_actionseq %s\n"
									"delete from %s where %s;",
									log_xid, log_actionseq,
									wd->tab_fqname[log_tableid], 
									log_cmddata);
							break;
					}
				}

				/*
				 * Now put all the line buffers back. Filled ones
				 * into the repldata, unused ones into the pool.
				 */
				pthread_mutex_lock(&(wd->workdata_lock));
				for (tupno = 0; tupno < alloc_lines; tupno++)
				{
					if (tupno < ntuples)
						DLLIST_ADD_TAIL(wd->repldata_head, wd->repldata_tail,
								data_line[tupno]);
					else
						DLLIST_ADD_HEAD(wd->linepool_head, wd->linepool_tail,
								data_line[tupno]);
				}
				if (ntuples > 0)
					pthread_cond_signal(&(wd->repldata_cond));
				if (ntuples < alloc_lines)
					pthread_cond_broadcast(&(wd->linepool_cond));
				pthread_mutex_unlock(&(wd->workdata_lock));

				if (ntuples < alloc_lines)
				{
					alloc_lines = 0;
					break;
				}

				alloc_lines = 0;
			}
		} while (0);

		/*
		 * if there are still line buffers allocated, give them back.
		 */
		if (alloc_lines > 0)
		{
			pthread_mutex_lock(&(wd->workdata_lock));
			while(alloc_lines > 0)
			{
				alloc_lines--;
				DLLIST_ADD_HEAD(wd->linepool_head, wd->linepool_tail,
						data_line[alloc_lines]);
			}
			pthread_cond_broadcast(&(wd->linepool_cond));
			pthread_mutex_unlock(&(wd->workdata_lock));
		}

		/*
		 * Close the cursor and rollback the transaction.
		 */
		slon_mkquery(&query, "close LOG; ");
		if (query_execute(node, dbconn, &query, PGRES_COMMAND_OK) < 0)
			errors++;
		slon_mkquery(&query, "rollback transaction; ");
		if (query_execute(node, dbconn, &query, PGRES_COMMAND_OK) < 0)
			errors++;

		/*
		 * Change our helper status to DONE and tell the worker 
		 * thread about it.
		 */
		pthread_mutex_lock(&(provider->helper_lock));
		provider->helper_status = SLON_WG_DONE;
		dstring_reset(&provider->helper_qualification);

		pthread_mutex_lock(&(wd->workdata_lock));
		while (wd->linepool_head == NULL)
		{
			pthread_cond_wait(&(wd->linepool_cond), &(wd->workdata_lock));
		}
		line = wd->linepool_head;
		DLLIST_REMOVE(wd->linepool_head, wd->linepool_tail, line);
		if (errors)
			line->code 	= SLON_WGLC_ERROR;
		else
			line->code 	= SLON_WGLC_DONE;
		line->provider	= provider;
		DLLIST_ADD_HEAD(wd->repldata_head, wd->repldata_tail, line);
		pthread_cond_signal(&(wd->repldata_cond));
		pthread_mutex_unlock(&(wd->workdata_lock));

		/*
		 * Wait for the whole workgroup to be done.
		 */
		while (provider->helper_status == SLON_WG_DONE)
		{
			slon_log(SLON_DEBUG1, "remoteHelperThread_%d_%d: "
					"waiting for workgroup to finish\n",
					node->no_id, provider->no_id);

			pthread_cond_wait(&(provider->helper_cond), &(provider->helper_lock));
		}
		pthread_mutex_unlock(&(provider->helper_lock));
	}
}


