/*  vim:set softtabstop=2 shiftwidth=2 tabstop=2 expandtab: */
/*
 * This file defines helpful method to manage encoding of the notification
 * message.
 */

#include <string>
#include <iostream>

#include "gnmi_encode.h"
#include "gnmi_stat.h"

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
void BuildNotification(
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

  /* TODO : Path aliases defined by clients to access a long path.
   * We need to provide global variable with the list of aliases which can be
   * used only if use_aliases boolean is true in SubscriptionList. */
  if (request.use_aliases())
    std::cerr << "Unsupported usage of aliases" << std::endl;

  /* TODO check if only updates should be sent
   * Require to implement a caching system to access last data sent. */
  if (request.updates_only())
    std::cerr << "Unsupported usage of Updates, every paths will be sent"
              << std::endl;

  /* Fill Update RepeatedPtrField in Notification message
   * Update field contains only data elements that have changed values. */
  for (int i=0; i<request.subscription_size(); i++) {
    Subscription sub = request.subscription(i);
    Update* update = updateList->Add();
    Path* path = update->mutable_path();
    TypedValue* val = update->mutable_val();

    /* TODO: Fetch the value from the stat_api instead of hardcoding a fake one
     * If succeeded Copy Request path into response path. */
    StatConnector stat = StatConnector();
    u8 **patterns = stat.CreatePatterns("/if");
    stat.FillCounter(val, patterns);

    path->CopyFrom(sub.path());
    update->set_duplicates(0);
  }

  notification->set_atomic(false);
}
