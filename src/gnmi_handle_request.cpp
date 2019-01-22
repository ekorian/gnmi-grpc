/*  vim:set softtabstop=2 shiftwidth=2 tabstop=2 expandtab: */

#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include <string>

#include <grpc/grpc.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include "../proto/gnmi.grpc.pb.h"
#include "gnmi_handle_request.h"

using namespace grpc;
using namespace gnmi;
using namespace std;
using namespace chrono;
using google::protobuf::RepeatedPtrField;

/* GnmiToUnixPath - Convert a GNMI Path to UNIX Path
 * @param path the Gnmi Path
 */
string GnmiToUnixPath(Path path)
{
  string uxpath;

  for (int i=0; i < path.elem_size(); i++) {
    uxpath += "/";
    uxpath += path.elem(i).name();
  }

  return uxpath;
}

/**
 * BuildNotification - build a Notification message to answer a SubscribeRequest.
 * @param request the SubscriptionList from SubscribeRequest to answer to.
 * @param response the SubscribeResponse that is constructed by this function.
 */
void RequestHandler::BuildNotification(
    const SubscriptionList& request, SubscribeResponse& response)
{
  Notification *notification = response.mutable_update();
  RepeatedPtrField<Update>* updateList = notification->mutable_update();
  milliseconds ts;

  /* Get time since epoch in milliseconds */
  ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
  notification->set_timestamp(ts.count());

  /* Notification message prefix based on SubscriptionList prefix */
  if (request.has_prefix()) {
    Path* prefix = notification->mutable_prefix();
    prefix->set_target(request.prefix().target());
    // set name of measurement
    prefix->mutable_elem()->Add()->set_name("measurement1");
  }

  // Defined refer to a long Path by a shorter one: alias
  if (request.use_aliases())
    cerr << "Unsupported usage of aliases" << endl;

  /* TODO check if only updates should be sent
   * Require to implement a caching system to access last data sent. */
  if (request.updates_only())
    cerr << "Unsupported usage of Updates, every paths will be sent"  << endl;

  /* Fill Update RepeatedPtrField in Notification message
   * Update field contains only data elements that have changed values. */
  for (int i = 0; i < request.subscription_size(); i++) {
    Subscription sub = request.subscription(i);

    // Fetch all found counters value for a requested path
    cout << "Requested: " + GnmiToUnixPath(sub.path()) << endl;
    statc.FillCounters(updateList, GnmiToUnixPath(sub.path()));
  }

  notification->set_atomic(false);
}

/**
 * Handles SubscribeRequest messages with STREAM subscription mode by
 * periodically sending updates to the client.
 */
Status RequestHandler::handleStream(
    ServerContext* context, SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  // Checks that sample_interval values are not higher than
  // std::chrono::duration<long long, std::nano>::max().count() = 9223372036854775807
  for (int i=0; i<request.subscribe().subscription_size(); i++) {
    Subscription sub = request.subscribe().subscription(i);
    if (sub.sample_interval() > duration<long long, std::nano>::max().count()) {
      context->TryCancel();
      return Status(StatusCode::INVALID_ARGUMENT, grpc::string(
        "sample_interval must be less than 9223372036854775807 nanoseconds"));
    }
  }

  // Sends a first Notification message that updates all Subcriptions
  SubscribeResponse response;
  BuildNotification(request.subscribe(), response);
  cout << response.DebugString() << endl;
  stream->Write(response);
  response.Clear();

  // Sends a SYNC message that indicates that initial synchronization
  // has completed, i.e. each Subscription has been updated once
  response.set_sync_response(true);
  cout << response.DebugString() << endl;
  stream->Write(response);
  response.Clear();

  // We use a vector of pairs instead of a map as we are going to iterate more
  // than we are going to retrieve specific keys.
  vector<pair<Subscription, time_point<high_resolution_clock>>> chronomap;
  for (int i=0; i<request.subscribe().subscription_size(); i++) {
    Subscription sub = request.subscribe().subscription(i);
    switch (sub.mode()) {
      case SAMPLE:
        chronomap.emplace_back(sub, high_resolution_clock::now());
        break;
      default:
        // TODO: Handle ON_CHANGE and TARGET_DEFINED modes
        // Ref: 3.5.1.5.2
        break;
    }
  }

  /* Periodically updates paths that require SAMPLE updates
   * Note : There is only one Path per Subscription, but repeated
   * Subscriptions in a SubscriptionList, each Subscription can
   * have its own sample interval */
  while(!context->IsCancelled()) {
    auto start = high_resolution_clock::now();

    SubscribeRequest updateRequest(request);
    SubscriptionList* updateList(updateRequest.mutable_subscribe());
    updateList->clear_subscription();

    for (auto& pair : chronomap) {
      duration<long long, std::nano> duration =
        high_resolution_clock::now()-pair.second;
      if (duration > nanoseconds{pair.first.sample_interval()}) {
        pair.second = high_resolution_clock::now();
        Subscription* sub = updateList->add_subscription();
        sub->CopyFrom(pair.first);
      }
    }

    if (updateList->subscription_size() > 0) {
      BuildNotification(updateRequest.subscribe(), response);
      cout << "Sending sampled update" << endl;
      cout << response.DebugString() << endl;
      stream->Write(response);
      response.Clear();
    }

    // Caps the loop at 5 iterations per second
    auto loopTime = high_resolution_clock::now() - start;
    this_thread::sleep_for(milliseconds(200) - loopTime);
  }

  return Status::OK;
}

/**
 * Handles SubscribeRequest messages with ONCE subscription mode by updating
 * all the Subscriptions once, sending a SYNC message, then closing the RPC.
 */
Status RequestHandler::handleOnce(
    ServerContext* context, SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  // Sends a Notification message that updates all Subcriptions once
  SubscribeResponse response;
  BuildNotification(request.subscribe(), response);
  cout << response.DebugString() << endl;
  stream->Write(response);
  response.Clear();

  // Sends a message that indicates that initial synchronization
  // has completed, i.e. each Subscription has been updated once
  response.set_sync_response(true);
  cout << response.DebugString() << endl;
  stream->Write(response);
  response.Clear();

  context->TryCancel();
  return Status::OK;
}

/**
 * Handles SubscribeRequest messages with POLL subscription mode by updating
 * all the Subscriptions each time a Poll request in received.
 */
Status RequestHandler::handlePoll(
    ServerContext* context, SubscribeRequest request,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  SubscribeRequest subscription = request;
  while (stream->Read(&request)) {
    switch (request.request_case()) {
      case request.kPoll:
        {
          // Sends a Notification message that updates all Subcriptions once
          SubscribeResponse response;
          BuildNotification(subscription.subscribe(), response);
          cout << response.DebugString() << endl;
          stream->Write(response);
          response.Clear();
          break;
        }
      case request.kAliases:
        return Status(StatusCode::UNIMPLEMENTED, grpc::string(
              "Aliases not implemented yet"));
      case request.kSubscribe:
        return Status(StatusCode::INVALID_ARGUMENT, grpc::string(
              "A SubscriptionList has already been received for this RPC"));
      default:
        return Status(StatusCode::INVALID_ARGUMENT, grpc::string(
              "Unknown content for SubscribeRequest message"));
    }
  }
  return Status::OK;
}

/**
 * Handles the first SubscribeRequest message.
 * If it does not have the "subscribe" field set, the RPC MUST be cancelled.
 * Ref: 3.5.1.1
 */
Status RequestHandler::handleSubscribeRequest(ServerContext* context,
    ServerReaderWriter<SubscribeResponse, SubscribeRequest>* stream)
{
  SubscribeRequest request;
  stream->Read(&request);
  if (!request.has_subscribe()) {
    context->TryCancel();
    return Status(StatusCode::INVALID_ARGUMENT, grpc::string(
          "SubscribeRequest needs non-empty SubscriptionList"));
  }

  switch (request.subscribe().mode()) {
    case SubscriptionList_Mode_STREAM:
      return handleStream(context, request, stream);
    case SubscriptionList_Mode_ONCE:
      return handleOnce(context, request, stream);
    case SubscriptionList_Mode_POLL:
      return handlePoll(context, request, stream);
    default:
      return Status(StatusCode::UNKNOWN,
          grpc::string("Unkown subscription mode"));
  }
  return Status::OK;
}
