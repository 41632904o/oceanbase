#include "common/ob_client_manager.h"
#include "ob_ms_async_rpc.h"
#include "ob_ms_sql_request.h"
#include "common/ob_spop_spush_queue.h"
#include "common/ob_atomic.h"
#include "common/ob_common_param.h"
#include "common/ob_malloc.h"

using namespace oceanbase::common;
using namespace oceanbase::mergeserver;

uint64_t ObMsSqlRequest::id_allocator_ = 0;

ObMsSqlRequest::ObMsSqlRequest()
  :buffer_pool_(ObModIds::OB_SQL_RPC_REQUEST),
  finish_(false)
{
  timeout_ = 0;
  rpc_timeout_percent_ = DEFAULT_TIMEOUT_PERCENT;
  read_param_ = NULL;
  finish_queue_inited_ = false;
  request_id_ = atomic_inc(reinterpret_cast<volatile uint64_t*>(&id_allocator_));
}
void ObMsSqlRequest::set_tablet_location_cache_proxy(common::ObTabletLocationCacheProxy * proxy)
{
  cache_proxy_ = proxy;
}
void ObMsSqlRequest::set_merger_async_rpc_stub(const ObMergerAsyncRpcStub * async_rpc)
{
  async_rpc_stub_ = async_rpc;
}

ObMsSqlRequest::~ObMsSqlRequest()
{
}

int ObMsSqlRequest::close()
{
  int ret = OB_SUCCESS;
  if (OB_SUCCESS != (ret = invalidate_event_in_waiting_queue()))
  {
    TBSYS_LOG(WARN, "fail to remove invalid event in wating queue. ret=%d", ret);
  }
  if (OB_SUCCESS != (ret = remove_invalid_event_in_finish_queue(timeout_)))
  {
    TBSYS_LOG(WARN, "fail to remove invalid event in finish queue. ret=%d", ret);
  }
  return ret;
}

int ObMsSqlRequest::init(const uint64_t count, const uint32_t mod_id,  ObSessionManager * session_mgr)
{
  int err = OB_SUCCESS;
  if (OB_SUCCESS != (err = finish_queue_.init(count, mod_id)))
  {
    finish_queue_inited_ = false;
    TBSYS_LOG(WARN,"fail to init finish_queue_ [err:%d]", err);
  }
  else
  {
    finish_queue_inited_ = true;
    session_mgr_ = session_mgr;
  }
  return err;
}

int ObMsSqlRequest::reset()
{
  timeout_ = 0;
  read_param_ = NULL;
  uint64_t count = 0;
  int ret = release_succ_event(count);
  if (ret != OB_SUCCESS)
  {
    TBSYS_LOG(WARN, "release all processed event failed:request[%lu], ret[%d]",
        request_id_, ret);
  }
  buffer_pool_.reset();
  finish_queue_.reset();
  // bugfix #224057, if queue is not clear, will cause deadlock
  waiting_queue_.clear();
  timeout_ = 0;
  rpc_timeout_percent_ = DEFAULT_TIMEOUT_PERCENT;
  read_param_ = NULL;
  finish_queue_inited_ = false;
  finish_ = false;
  return ret;
}
void ObMsSqlRequest::alloc_request_id()
{
  request_id_ = atomic_inc(reinterpret_cast<volatile uint64_t*>(&id_allocator_));
}
uint64_t ObMsSqlRequest::get_request_id(void) const
{
  return request_id_;
}

const ObMergerAsyncRpcStub * ObMsSqlRequest::get_rpc(void) const
{
  return async_rpc_stub_;
}

void ObMsSqlRequest::set_timeout(const int64_t timeout)
{
  timeout_ = timeout;
}

void ObMsSqlRequest::set_request_param(const ObReadParam * param, const int64_t timeout)
{
  read_param_ = param;
  timeout_ = timeout;
}

int64_t ObMsSqlRequest::get_timeout(void) const
{
  return timeout_;
}


int ObMsSqlRequest::get_timeout_percent(void) const 
{
  return rpc_timeout_percent_;
}

void ObMsSqlRequest::set_timeout_percent(const int32_t percent)
{
  rpc_timeout_percent_ = percent;
}

// create and add a new rpc event to the wait queue
int ObMsSqlRequest::create(ObMsSqlRpcEvent ** event)
{
  int ret = OB_SUCCESS;
  if (NULL == event)
  {
    ret = OB_INPUT_PARAM_ERROR;
    TBSYS_LOG(WARN, "check input failed:request[%lu], event[%p]", request_id_, event);
  }
  else
  {
    ret = create_rpc_event(event);
    if ((OB_SUCCESS != ret) || (NULL == *event))
    {
      ret = OB_ERROR;
      TBSYS_LOG(WARN, "create rpc event failed:request[%lu], event[%p], ret[%d]",
          request_id_, event, ret);
    }
    else
    {
      (*event)->set_timestamp(tbsys::CTimeUtil::getTime());
      tbsys::CThreadGuard lock(&lock_);
      ret = waiting_queue_.push_back(*event);
      if (ret != OB_SUCCESS)
      {
        TBSYS_LOG(ERROR, "push new event to waiting queue failed:request[%lu], event[%lu], "
            "client[%lu], ret[%d]", request_id_, (*event)->get_event_id(),
            (*event)->get_client_id(), ret);
      }
      else
      {
        TBSYS_LOG(DEBUG, "push new event succ:request[%lu], event[%lu], client[%lu]",
            request_id_, (*event)->get_event_id(), (*event)->get_client_id());
      }
    }
  }
  return ret;
}

int ObMsSqlRequest::destroy(ObMsSqlRpcEvent * event)
{
  int ret = OB_SUCCESS;
  if (NULL == event)
  {
    ret = OB_INPUT_PARAM_ERROR;
    TBSYS_LOG(WARN, "check input failed:request[%lu], event[%p]", request_id_, event);
  }
  else
  {
    bool is_valid = true;
    ret = remove_wait_queue(event, is_valid);
    if ((ret != OB_SUCCESS) || (false == is_valid))
    {
      TBSYS_LOG(ERROR, "remove from the waiting queue failed:clinet[%lu], event[%lu], "
          "request[%lu], valid[%d], ret[%d]", event->get_client_id(), event->get_event_id(),
          request_id_, is_valid, ret);
    }
    destroy_rpc_event(event);
  }
  return ret;
}

// by the tbnet thread so lightweight
int ObMsSqlRequest::signal(ObMsSqlRpcEvent & event)
{
  // only push to the finish_queue not check request id or in wait queue
  int ret = OB_SUCCESS;
  if (request_id_ != event.get_client_id())
  {
    TBSYS_LOG(INFO , "the event we do not expect from server, "
        "event[%ld], client[%ld], current client[%ld], session id[%ld], rcode[%d]",
        event.get_event_id(), event.get_client_id(), request_id_,
        event.get_session_id(), event.get_result_code());
    ret = OB_INVALID_ERROR;
  }
  else
  {
    tbsys::CThreadGuard lock(&flock_);
    ret = finish_queue_.push(&event);
  }

  if (ret != OB_SUCCESS)
  {
    if (OB_INVALID_ERROR != ret)
    {
      TBSYS_LOG(ERROR, "push to finish queue failed:client[%lu], event[%lu], "
          "request[%lu], finish_size[%ld], ret[%d]", event.get_client_id(), event.get_event_id(),
          request_id_, finish_queue_.size(), ret);
    }
    else if (OB_INVALID_ERROR == ret && OB_SUCCESS == event.get_result_code()
        && event.get_session_id() >  ObCommonSqlRpcEvent::INVALID_SESSION_ID)
    {
      terminate_remote_session(event.get_server(), event.get_session_id());
      TBSYS_LOG(INFO,"end unexpect session [client:%ld, session_id:%lu]",
          event.get_client_id(), event.get_session_id());
    }
    else
    {
      TBSYS_LOG(ERROR, "push to finish queue failed. destroy it:client[%lu], event[%lu], "
          "request[%lu], finish_size[%ld], ret[%d]", event.get_client_id(), event.get_event_id(),
          request_id_, finish_queue_.size(), ret);
    }
    bool is_valid = false;
    remove_wait_queue(&event, is_valid);
  }
  else
  {
    TBSYS_LOG(DEBUG, "push to finish queue succ:client[%lu], event[%lu], request[%lu]",
        event.get_client_id(), event.get_event_id(), request_id_);
  }
  return ret;
}

const ObReadParam * ObMsSqlRequest::get_request_param(int64_t & timeout) const
{
  timeout = timeout_;
  return read_param_;
}

int ObMsSqlRequest::process_rpc_event(const int64_t timeout, ObMsSqlRpcEvent * event, bool & finish)
{
  bool is_valid = false;
  int ret = remove_wait_queue(event, is_valid);
  if (ret != OB_SUCCESS)
  {
    // not find from the wait queue
    destroy_rpc_event(event);
    TBSYS_LOG(ERROR, "remove the event failed:request[%lu], ret[%d]", request_id_, ret);
  }
  else if (true == is_valid)
  {
    // must push to the result queue no matter the event is succ or not
    ret = result_queue_.push_back(event);
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(ERROR, "push the event to result queue failed:client[%lu], request[%lu], "
          "event[%lu], ret[%d]", event->get_client_id(), request_id_, event->get_event_id(), ret);
      // destory the rpc event
      destroy_rpc_event(event);
    }
    else
    {
      TBSYS_LOG(DEBUG, "push the event to result succ:client[%lu], request[%lu], event[%lu]",
          event->get_client_id(), request_id_, event->get_event_id());
      ret = process_result(timeout, event, finish);
      if (ret != OB_SUCCESS)
      {
        TBSYS_LOG(WARN, "process the result failed:client[%lu], request[%lu], event[%lu], ret[%d]",
            event->get_client_id(), request_id_, event->get_event_id(), ret);
      }
    }
  }
  else
  {
    // the event is not valid so destory the rpc event
    TBSYS_LOG(WARN, "the event is not valid:client[%lu], event[%lu], request[%ld], valid[%d]",
        event->get_client_id(), event->get_event_id(), request_id_, is_valid);
    destroy_rpc_event(event);
  }
  return ret;
}

int ObMsSqlRequest::remove_wait_queue(ObMsSqlRpcEvent * event, bool & is_valid)
{
  is_valid = false;
  int ret = OB_SUCCESS;
  if (NULL == event)
  {
    ret = OB_INPUT_PARAM_ERROR;
    TBSYS_LOG(WARN, "check rpc event failed:request[%lu], event[%p]", request_id_, event);
  }
  else
  {
    tbsys::CThreadGuard lock(&lock_);
    ret = waiting_queue_.erase(event);
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(ERROR, "find event not in waiting queue:client[%lu], event[%lu], result[%d], "
          "request[%lu], ret[%d]", event->get_client_id(), event->get_event_id(),
          event->get_result_code(), request_id_, ret);
    }
    else if (event->get_client_id() != request_id_)
    {
      TBSYS_LOG(WARN, "check the event client id not equal with request:"
          "client[%lu], event[%lu], result[%d], request[%lu]", event->get_client_id(),
          event->get_event_id(), event->get_result_code(), request_id_);
      /// destroy_rpc_event(event);
    }
    else
    {
      is_valid = true;
    }
  }
  return ret;
}

int ObMsSqlRequest::wait(const int64_t timeout)
{
  int ret = OB_SUCCESS;
  int64_t cur_timestamp = tbsys::CTimeUtil::getTime();
  int64_t last_timestamp = cur_timestamp + timeout;
  int64_t remainder = timeout;
  bool finish = false;
  ObMsSqlRpcEvent * event = NULL;
  while (remainder > 0)
  {
    if (OB_SUCCESS == (ret = finish_queue_.pop(remainder, (void *&)event)))
    {
      /// process a new finished event
      ret = process_rpc_event(remainder, event, finish);
      if (ret != OB_SUCCESS)
      {
        TBSYS_LOG(WARN, "process rpc event failed:request[%lu], event[%p], ret[%d]",
            request_id_, event, ret);
        break;
      }
      else if (true == finish)
      {
        ret = OB_SUCCESS;
        break;
      }
    }
    else
    {
      TBSYS_LOG(WARN, "pop result from finish queue failed:request[%lu], event[%p], ret[%d]",
          request_id_, event, ret);
    }
    // update the remainder time
    cur_timestamp = tbsys::CTimeUtil::getTime();
    remainder = last_timestamp - cur_timestamp;
    TBSYS_LOG(DEBUG, "check remainder for next wait:request[%lu], cur[%ld], last[%ld], remainder[%ld]",
        request_id_, cur_timestamp, last_timestamp, remainder);
    if ((NULL != session_mgr_) && !session_mgr_->session_alive(session_id_))
    {
      ret = OB_SESSION_KILLED;
      TBSYS_LOG(INFO, "session was killed [session_id:%u]", session_id_);
      break;
    }
  }

  // wait finished, switch to next request and clean current queue_
  request_id_ = atomic_inc(reinterpret_cast<volatile uint64_t*>(&id_allocator_));
  TBSYS_LOG(DEBUG, "check all the event finish:request[%lu], remain finish queue size[%lu]",
      request_id_, finish_queue_.size());
  remove_invalid_event_in_finish_queue(remainder);

  if (remainder <= 0)
  {
    ret = OB_RESPONSE_TIME_OUT;
    TBSYS_LOG(WARN, "check remainder less than zero:request[%lu], remainder[%ld]",
      request_id_, remainder);
  }
  return ret;
}

// worker thread release all the events
int ObMsSqlRequest::release_succ_event(uint64_t & event_count)
{
  int ret = OB_SUCCESS;
  ObMsSqlRpcEvent * rpc = NULL;
  event_count = result_queue_.size();
  TBSYS_LOG(DEBUG, "relase all the succ event:request[%lu], count[%lu], finish_size[%lu], result_size[%lu]",
            request_id_, event_count, finish_queue_.size(), waiting_queue_.size());
  for (uint64_t i = 0; i < event_count; ++i)
  {
    rpc = result_queue_[static_cast<int32_t>(i)];
    if (NULL == rpc)
    {
      ret = OB_INNER_STAT_ERROR;
      TBSYS_LOG(ERROR, "check finished result failed:request[%lu], index[%lu], event[%p]",
          request_id_, i, rpc);
    }
    else
    {
      destroy_rpc_event(rpc);
      result_queue_[static_cast<int32_t>(i)] = NULL;
    }
  }
  // clear all the finished result queue
  result_queue_.clear();
  return ret;
}

int ObMsSqlRequest::create_rpc_event(ObMsSqlRpcEvent ** event)
{
  int ret = OB_SUCCESS;
  /// ObMsSqlRpcEvent * rpc = new(std::nothrow) ObMsSqlRpcEvent;
  ObMsSqlRpcEvent * rpc = reinterpret_cast<ObMsSqlRpcEvent *>(ob_malloc(sizeof(ObMsSqlRpcEvent),
        ObModIds::OB_MS_RPC_EVENT));
  if (NULL == rpc)
  {
    ret = OB_ERROR;
    TBSYS_LOG(WARN, "check rpc event allocate failed:rpc[%p]", rpc);
  }
  else
  {
    new (rpc) ObMsSqlRpcEvent;
    TBSYS_LOG(DEBUG, "create rpc event succ:request[%lu], event[%lu], ptr[%p]",
        request_id_, rpc->get_event_id(), rpc);
    ret = rpc->init(request_id_, this);
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(WARN, "init rpc event failed:request[%lu], event[%lu], ptr[%p], ret[%d]",
          request_id_, rpc->get_event_id(), rpc, ret);
      destroy_rpc_event(rpc);
    }
    else
    {
      *event = rpc;
    }
  }
  return ret;
}

void ObMsSqlRequest::destroy_rpc_event(ObMsSqlRpcEvent * rpc)
{
  if (NULL != rpc)
  {
    rpc->lock();
    TBSYS_LOG(DEBUG, "destroy rpc event succ:client[%lu], request[%lu], event[%lu], "
        "result[%d], ptr[%p]", rpc->get_client_id(), request_id_, rpc->get_event_id(),
        rpc->get_result_code(), rpc);
    /// delete rpc;
    rpc->~ObCommonSqlRpcEvent();
    ob_free(rpc, ObModIds::OB_MS_RPC_EVENT);
    rpc = NULL;
  }
  else
  {
    TBSYS_LOG(WARN, "check destroy rpc event failed:request[%lu], event[%p]",
        request_id_, rpc);
  }
}

void ObMsSqlRequest::print_info(FILE * file) const
{
  if (NULL != file)
  {
    // no need lock
    fprintf(file, "merger request event:allocator[%lu], id[%lu], wait[%ld], finish[%ld], succ[%d]\n",
        ObMsSqlRequest::id_allocator_, request_id_, waiting_queue_.size(), finish_queue_.size(),
        result_queue_.size());
    // print waiting event list
    // print finished event list
    // print processed event list
    fflush(file);
  }
}

int ObMsSqlRequest::update_location_cache(const ObServer &svr, const int32_t result_code, const ObSqlScanParam & scan_param)
{
  int err = OB_SUCCESS;
  ObTabletLocationItem addr;
  addr.server_.chunkserver_ = svr;
  ObTabletLocationList loc_list;
  loc_list.set_buffer(buffer_pool_);
  if (OB_CS_TABLET_NOT_EXIST == result_code)
  {
    // to prevent rootserver failure, should not del cache in advance.
    // try force renew is a good choice, if fail, use old cache value, if succ, great!
    if (OB_SUCCESS != (err = cache_proxy_->set_item_invalid(scan_param, addr, loc_list)))
    {
      TBSYS_LOG(WARN,"fail to invalidate cache item [err:%d], err", err);
    }
    else if (OB_SUCCESS != (err = cache_proxy_->renew_location_item(scan_param, loc_list, true)))
    {
      TBSYS_LOG(WARN, "fail to force renew location cache item. err=%d", err);
    }
  }
  else if (OB_NOT_MASTER == result_code)
  {
    //do nothing
  }
  else if (OB_SUCCESS != result_code)
  {
    if (OB_SUCCESS != (err = cache_proxy_->server_fail(scan_param, loc_list, svr)))
    {
      TBSYS_LOG(WARN,"fail to update cache item [err:%d], err", err);
    }
  }
  return err;
}

int ObMsSqlRequest::update_location_cache(const ObServer &svr, const int32_t result_code, const ObCellInfo & cell)
{
  int err = OB_SUCCESS;
  ObTabletLocationItem addr;
  addr.server_.chunkserver_ = svr;
  ObTabletLocationList loc_list;
  loc_list.set_buffer(buffer_pool_);
  if (OB_CS_TABLET_NOT_EXIST == result_code)
  {
    if (OB_SUCCESS != (err = cache_proxy_->set_item_invalid(cell.table_id_, cell.row_key_, addr, loc_list)))
    {
      TBSYS_LOG(WARN,"fail to invalidate cache item [err:%d], err", err);
    }
  }
  else if (OB_SUCCESS != result_code)
  {
    if (OB_SUCCESS != (err = cache_proxy_->server_fail(cell.table_id_, cell.row_key_, loc_list, svr)))
    {
      TBSYS_LOG(WARN,"fail to update cache item [err:%d], err", err);
    }
  }
  return err;
}

int ObMsSqlRequest::update_location_cache(const ObServer &svr, const int32_t result_code,
    const uint64_t table_id, const ObRowkey &row_key)
{
  int err = OB_SUCCESS;
  ObTabletLocationItem addr;
  addr.server_.chunkserver_ = svr;
  ObTabletLocationList loc_list;
  loc_list.set_buffer(buffer_pool_);
  if (OB_CS_TABLET_NOT_EXIST == result_code)
  {
    // to prevent rootserver failure, should not del cache in advance.
    // try force renew is a good choice, if fail, use old cache value, if succ, great!
    if (OB_SUCCESS != (err = cache_proxy_->set_item_invalid(table_id, row_key, addr, loc_list)))
    {
      TBSYS_LOG(WARN,"fail to invalidate cache item [err:%d], err", err);
    }
    else if (OB_SUCCESS != (err = cache_proxy_->renew_location_item(table_id, row_key, loc_list, true)))
    {
      TBSYS_LOG(WARN, "fail to force renew location cache item. err=%d", err);
    }
  }
  else if (OB_SUCCESS != result_code)
  {
    if (OB_SUCCESS != (err = cache_proxy_->server_fail(table_id, row_key, loc_list, svr)))
    {
      TBSYS_LOG(WARN,"fail to update cache item [err:%d], err", err);
    }
  }
  return err;
}


int ObMsSqlRequest::terminate_remote_session(const common::ObServer& server, const int64_t session_id)
{
  const static int32_t end_session_req_timeout = 100000;
  ObDataBuffer in_buffer;
  int ret = OB_SUCCESS;

  const ObMergerAsyncRpcStub * async_rpc_stub = get_rpc();
  if (NULL != async_rpc_stub && session_id > ObCommonSqlRpcEvent::INVALID_SESSION_ID)
  {
    ret = async_rpc_stub->get_client_manager()->post_end_next(
        server, session_id, end_session_req_timeout, in_buffer, NULL, NULL);
  }
  else
  {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

int ObMsSqlRequest::remove_invalid_event_in_finish_queue(const int64_t timeout)
{
  int ret = OB_SUCCESS;
  int64_t remainder = timeout;
  int64_t size = finish_queue_.size();
  bool is_valid = false;
  ObMsSqlRpcEvent * event = NULL;
  while (remainder > 0 && size > 0)
  {
    if (OB_SUCCESS == (ret = finish_queue_.pop(remainder, (void *&)event)))
    {
      if (NULL != event && OB_SUCCESS == event->get_result_code()
          && event->get_session_id() >  ObCommonSqlRpcEvent::INVALID_SESSION_ID)
      {
        terminate_remote_session(event->get_server(), event->get_session_id());
        TBSYS_LOG(INFO,"end finished but not process session [client:%ld, session_id:%lu, event_id:%ld]",
            event->get_client_id(), event->get_session_id(), event->get_event_id());
      }
      remove_wait_queue(event, is_valid);
      destroy_rpc_event(event);
    }
    else
    {
      TBSYS_LOG(WARN, "pop result from finish queue failed:request[%lu], event[%p], ret[%d]",
          request_id_, event, ret);
    }
    size = finish_queue_.size();
  }
  return ret;
}


int ObMsSqlRequest::invalidate_event_in_waiting_queue()
{
  int ret = OB_SUCCESS;
  //ObMsSqlRpcEvent * event = NULL;
  ObList<ObMsSqlRpcEvent * >::iterator iter = waiting_queue_.begin();
  for (; iter != waiting_queue_.end(); iter++)
  {
    (*iter)->invalidate();
  }
  return ret;
}

// those request which has sent out but not returned
int64_t ObMsSqlRequest::get_waiting_queue_size()
{
  return waiting_queue_.size();
}

int64_t ObMsSqlRequest::get_finish_queue_size()
{
  return finish_queue_.size();
}

// TODO
bool ObMsSqlRequest::is_finish()
{
  return finish_;
}

int ObMsSqlRequest::wait_single_event(int64_t &timeout)
{
  int ret = OB_SUCCESS;
  int64_t cur_timestamp = tbsys::CTimeUtil::getTime();
  int64_t last_timestamp = cur_timestamp + timeout;
  int64_t &remainder = timeout;
  ObMsSqlRpcEvent * event = NULL;

  TBSYS_LOG(DEBUG, "wait_single_event:timeout:%ld", timeout);

  do{
    if (OB_SUCCESS == (ret = finish_queue_.pop(remainder, (void *&)event)))
    {
      /// process a new finished event
      ret = process_rpc_event(remainder, event, finish_);
      if (ret != OB_SUCCESS)
      {
        TBSYS_LOG(WARN, "process rpc event failed:request[%lu], event[%p], ret[%d]",
            request_id_, event, ret);
        break;
      }
      else if (true == finish_)
      {
        ret = OB_SUCCESS;
        break;
      }
    }
    else
    {
      TBSYS_LOG(WARN, "pop result from finish queue failed:request[%lu], event[%p], ret[%d]",
          request_id_, event, ret);
    }
    // update the remainder time
    cur_timestamp = tbsys::CTimeUtil::getTime();
    remainder = last_timestamp - cur_timestamp;
    TBSYS_LOG(DEBUG, "check remainder for next wait:request[%lu], cur[%ld], last[%ld], remainder[%ld]",
        request_id_, cur_timestamp, last_timestamp, remainder);
    if ((NULL != session_mgr_) && !session_mgr_->session_alive(session_id_))
    {
      ret = OB_SESSION_KILLED;
      TBSYS_LOG(INFO, "session was killed [session_id:%u]", session_id_);
      break;
    }
  }while(0);

  TBSYS_LOG(DEBUG, "timeout:%ld", timeout);

  // wait finished, switch to next request and clean current queue_
  //request_id_ = atomic_inc(reinterpret_cast<volatile uint64_t*>(&id_allocator_));
  TBSYS_LOG(DEBUG, "check all the event finish:request[%lu], remain finish queue size[%lu]",
      request_id_, finish_queue_.size());
  //remove_invalid_event_in_finish_queue(remainder);

  if (remainder <= 0)
  {
    ret = OB_RESPONSE_TIME_OUT;
    TBSYS_LOG(WARN, "check remainder less than zero:request[%lu], remainder[%ld]",
      request_id_, remainder);
  }
  return ret;
}
