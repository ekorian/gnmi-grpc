// vim: softtabstop=2 shiftwidth=2 tabstop=2 expandtab:

#include <iostream>
#include <vector>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

extern "C" {
#include <vpp-api/client/stat_client.h>
}

#include "gnmi_stat.h"

using namespace std;
using namespace gnmi;

/**
 * split - split string in substrings according to delimitor.
 * @param str the string to parse.
 * @param delim the dilimitation character.
 */
vector<string> split(const string &str, const char &delim)
{
  typedef string::const_iterator iter;
  iter beg = str.begin();
  vector<string> tokens;

  while(beg != str.end()) {
    iter temp = find(beg, str.end(), delim);
    if(beg != str.end() && !string(beg,temp).empty())
      tokens.push_back(string(beg, temp));
    beg = temp;
    while ((beg != str.end()) && (*beg == delim))
      beg++;
  }

  return tokens;
}

/**
 * UnixtoGnmiPath - Convert a Unix Path to a GNMI Path.
 * @param unixp Unix path.
 * @param path Pointer to GNMI path.
 */
void UnixToGnmiPath(string unixp, Path* path)
{
  vector<string> entries = split (unixp, '/');

  for (auto const& entry : entries) {
    PathElem *pathElem = path->add_elem();
    pathElem->set_name(entry);
  }
}

/* createPatterns - Create a VPP vector containing set of paths as parameter for
 * stat_segment_ls.
 * @param metric UNIX paths to shared memory stats counters.
 * @return VPP vector containing UNIX paths or NULL in case of failure.
 */
u8 ** createPatterns(string metric)
{
  u8 **patterns = 0;

  patterns = stat_segment_string_vector(patterns, metric.c_str());
  if (!patterns)
    exit(-ENOMEM);

  return patterns;
}

/* freePatterns - Free a VPP vector created with CreatePatterns.
 * @param patterns VPP vector containing UNIX path of stats counters.
 */
void freePatterns(u8 **patterns)
{
  stat_segment_vec_free(patterns);
}

/** FillCounter - Fill val with counter value collected with STAT API
 * @param val counter value answered to gNMI client
 * @param patterns VPP vector containing UNIX path of stats counter.
 */
void StatConnector::FillCounters(RepeatedPtrField<Update> *list, string metric)
{
  stat_segment_data_t *r;
  u8 ** patterns = createPatterns(metric);
  u32 *stats = 0;

  do {
    stats = stat_segment_ls(patterns);
    if (!stats) {
      cerr << "No pattern was found" << endl;
      return;
    }

    r = stat_segment_dump(stats);
  } while (r == 0); /* Memory layout has changed */

  // Iterate over all subdirectories of requested path
  for (int i = 0; i < stat_segment_vec_len(r); i++) {
    Update* update = list->Add();
    // Answer with found path instead of requested path
    UnixToGnmiPath(r[i].name, update->mutable_path());
    TypedValue* val = update->mutable_val();
    update->set_duplicates(0);

    switch (r[i].type) {
      case STAT_DIR_TYPE_COUNTER_VECTOR_SIMPLE:
        {
          string tmp;
          int k = 0, j = 0;
          for (; k < stat_segment_vec_len(r[i].simple_counter_vec); k++)
            for (; j < stat_segment_vec_len(r[i].simple_counter_vec[k]); j++)
              tmp += to_string(r[i].simple_counter_vec[k][j]);
          val->set_string_val(tmp);
          break;
        }
      case STAT_DIR_TYPE_COUNTER_VECTOR_COMBINED:
        {
          string tmp;
          int k = 0, j = 0;
          for (; k < stat_segment_vec_len(r[i].combined_counter_vec); k++)
            for (; j < stat_segment_vec_len(r[i].combined_counter_vec[k]); j++)
            {
              tmp += to_string(r[i].combined_counter_vec[k][j].packets);
              tmp.append(" ");
              tmp += to_string(r[i].combined_counter_vec[k][j].bytes);
            }
          val->set_string_val(tmp);
          break;
        }
      case STAT_DIR_TYPE_ERROR_INDEX:
        val->set_int_val(r[i].error_value);
        break;
      case STAT_DIR_TYPE_SCALAR_INDEX:
        val->set_int_val(r[i].scalar_value);
        break;
      default:
        cerr << "Unknown value" << endl;
    }
  }
}

/** Connect to VPP STAT API */
StatConnector::StatConnector()
{
  char socket_name[] = STAT_SEGMENT_SOCKET_FILE;
  int rc;

  rc = stat_segment_connect(socket_name);
  if (rc < 0) {
    cerr << "can not connect to VPP STAT unix socket" << endl;
    exit(1);
  }
}

/** Disconnect from VPP STAT API */
StatConnector::~StatConnector()
{
  stat_segment_disconnect();
  cout << "Disconnect STAT socket" << endl;
}

//////////////////////////////////////////////////////////////

/* Connect - Connect to VPP to use VPP API */
VapiConnector::VapiConnector() {
  string app_name = "gnmi_server";
  char *api_prefix = nullptr;
  const int max_req = 32; /* max outstanding requests */
  const int response_queue_size = 32;
  vapi_error_e rv;

  rv = con.connect(app_name.c_str(), api_prefix, max_req, response_queue_size);
  if (rv != VAPI_OK) {
    cerr << "Error connecting to VPP API" << endl;
    exit(1);
  }
}

/* Disconnect - Disconnect from VPP API */
VapiConnector::~VapiConnector() {
  con.disconnect();
}

/* GetInterfaceDetails - RPC client interacting directly with VPP binary API to
 * collect interfaces names to send in telemetry messages. */
void VapiConnector::GetInterfaceDetails() {
  vapi_error_e rv;

  //Dump: requ=vapi_msg_sw_interface_dump; resp=vapi_msg_sw_interface_details
  vapi::Sw_interface_dump req(con);

  rv = req.execute(); //send request
  if (rv != VAPI_OK)
    cerr << "request error" << endl;

  con.wait_for_response(req);
  for (auto& ifMsg : req.get_result_set()) {
    cout << "sw_if_index: "    << ifMsg.get_payload().sw_if_index << "\n"
         << "interface_name: " << ifMsg.get_payload().interface_name << "\n"
         << "MAC address: "    << ifMsg.get_payload().l2_address
         << endl;
  }
}

/* RegisterIfaceEvent - Ask for interface events using Want_interface_event
 * messages. Interface events are sent when an interface is created but not
 * when deleted. */
void VapiConnector::RegisterIfaceEvent() {
  vapi_error_e rv;

  /* Event registering: req:vapi_msg_want_interface_events;
   * resp: vapi_msg_want_interface_events_reply */
  vapi::Want_interface_events req(con);

  // Enable events and fill PID request field
  req.get_request().get_payload().pid = getpid();
  req.get_request().get_payload().enable_disable = 1;

  rv = req.execute(); // send request
  if (rv != VAPI_OK)
    cerr << "request error" << endl;

  con.wait_for_response(req);

  cout << "retvalue: " << req.get_response().get_payload().retval << endl;
}

/* Callback for Sw_interface_event */
vapi_error_e notify(if_event& ev) {
  cout << "Reeeeeeeeeeeeeeeeeeceeeeeeeeeeeeeeeived" << endl;

  for (auto& ifMsg : ev.get_result_set()) {
    cout << "id: "            << ifMsg.get_payload()._vl_msg_id << "\n"
         << "sw_if_index: "   << ifMsg.get_payload().sw_if_index << "\n"
         << "admin up/down: " << ifMsg.get_payload().admin_up_down << "\n"
         << "link up/down: "  << ifMsg.get_payload().link_up_down << "\n"
         << "deleted: "       << ifMsg.get_payload().deleted << "\n"
         << endl;
  }

  return (VAPI_OK);
}

/* DisplayIfaceEvent - This must run inside a thread */
void VapiConnector::DisplayIfaceEvent() {
  if_event ev(con, notify);
  con.dispatch(ev);
}

//For testing purpose only
/*
int main (int argc, char **argv)
{
  u8 **patterns = 0;
  string metric{"/if"};
  char socket_name[] = STAT_SEGMENT_SOCKET_FILE;
  int rc;

  rc = stat_segment_connect(socket_name);
  if (rc < 0) {
  cerr << "can not connect to VPP STAT unix socket" << endl;
  exit(1);
  }
  cout << "Connected to STAT socket" << endl;

  patterns = CreatePatterns(metric);
  if (!patterns)
  return -ENOMEM;
  DisplayPatterns(patterns);
  FreePatterns(patterns);

  // VPP API functions
  vapi::Connection con;
  Connect(con);
  GetInterfaceDetails(con);
  RegisterIfaceEvent(con);
  DisplayIfaceEvent(con);
  cout << "start sleeping" << endl;
  Disconnect(con);

  stat_segment_disconnect();
  cout << "Disconnect STAT socket" << endl;

  return 0;
} */
