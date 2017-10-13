// TODO: Cut out everything about fan-in boards once we've tested with
// some Double Chooz data. As per Camillo: "in protodune no fan in boxes
// so just skip that."

#include "USBstream-TypeDef.h"
#include "USBstream.h"
#include "USBstreamUtils.h"

// XXX Is there a motivation to use ROOT's thread library and not a standard
// one?  TThread is poorly documented, without even general explanation at
// https://root.cern.ch/doc/master/classTThread.html
#include "TThread.h"

#include <errno.h>
#include <syslog.h>
#include <algorithm>
#include <sys/statvfs.h>
#include <mysql++.h>

using namespace std;

#define BUFSIZE 1024

enum RunMode {kNormal, kRecovery, kReprocess};
enum TriggerMode {kNone, kSingleLayer, kDoubleLayer};

// Just little convenience structs for reading from the database
struct usb_sbop{
  int serial, board, offset, pmtboard_u;
  usb_sbop(const int serial_, const int board_,
           const int offset_, const int pmtboard_u_)
  {
    serial = serial_;
    board = board_;
    offset = offset_;
    pmtboard_u = pmtboard_u_;
  }
};

struct some_run_info{
  int daqdisk, ebcomment, ebsubrun;
  bool has_ebcomment, has_ebsubrun, has_stoptime;
};

// Consts
static const int maxUSB=10; // Maximum number of USB streams
static const int latency=5; // Seconds before DAQ switches files.
                            // FixME: 5 anticipated for far detector
static const int timestampsperoutput = 5; // XXX what?
static const int numChannels=64; // Number of channels in M64
static const int maxModules=64; // Maximum number of modules PER USB
                                // (okay if less than total number of modules)
static const int MAXTIME=60; // Seconds before timeout looking for baselines
                             // and binary data
static const int ENDTIME=5; // Number of seconds before time out at end of run
static const int SYNC_PULSE_CLK_COUNT_PERIOD_LOG2=29; // trigger system emits
                                                      // sync pulse at 62.5MHz

// Mutated as program runs
static int OV_EB_State = 0;
static bool finished=false; // Flag for joiner thread
static TThread *gThreads[maxUSB]; // An array of threads to decode files
static TThread *joinerThread; // Joiner thread for decoding
static int initial_delay = 0;
static vector<string> files; // vector to hold file names

// Set in parse_options()
static int Threshold = 73; //default 1.5 PE threshold
static string RunNumber = "";
static string OVRunType = "P";
static string OVDAQHost = "dcfovdaq";
static int OutDisk=1; // default output location of OV Ebuilder: /data1
static TriggerMode EBTrigMode = kDoubleLayer; // double-layer threshold

// Set in read_summary_table() just to get the other stuff
static char server[BUFSIZE] = {0};
static char username[BUFSIZE] = {0};
static char password[BUFSIZE] = {0};
static char database[BUFSIZE] = {0};

// Set in read_summary_table() from database information and used throughout
static int numUSB = 0;
static int num_nonFanUSB = 0;
static map<int, int> Datamap; // Maps numerical ordering of non-Fan-in
                             // USBs to all numerical ordering of all USBs
static map<int, int*> PMTOffsets; // Map to hold offsets for each PMT board
static map<int, int> PMTUniqueMap; // Maps 1000*USB_serial + board_number
                                  // to pmtboard_u in MySQL table
static USBstream OVUSBStream[maxUSB]; // An array of USBstream objects
static string BinaryDir; // Path to data
static string OutputFolder; // Default output data path hard-coded
static long int EBcomment = 0;
static int SubRunCounter = 0;

// *Size* set in read_summary_table()
static bool *overflow; // Keeps track of sync overflows for all boards
static long int *maxcount_16ns_lo; // Keeps track of max clock count
static long int *maxcount_16ns_hi; // for sync overflows for all boards

// set in read_summary_table() and main()
static RunMode EBRunMode = kNormal;

static void write_ebretval(const int val)
{
  return; // XXX No database for now

  mysqlpp::Connection myconn(false); // false to not throw exceptions on errors

  if(!myconn.connect(database, server, username, password)) {
    log_msg(LOG_WARNING, "Cannot connect to MySQL database %s at %s\n",
      database, server);
    return;
  }

  char query_string[BUFSIZE];

  sprintf(query_string, "SELECT Run_number FROM OV_runsummary "
    "WHERE Run_number = '%s';", RunNumber.c_str());
  mysqlpp::Query query = myconn.query(query_string);
  mysqlpp::StoreQueryResult res = query.store();

  if(res.num_rows() == 1){ // Run has never been reprocessed with this configuration
    sprintf(query_string, "Update OV_runsummary set EBretval = '%d' "
      "where Run_number = '%s';", val, RunNumber.c_str());

    mysqlpp::Query query2 = myconn.query(query_string);
    if(!query2.execute())
      log_msg(LOG_WARNING, "MySQL query (%s) error: %s\n", query_string, query2.error());
  }
  else {
    log_msg(LOG_WARNING,
      "Did not find unique OV_runsummary entry for run %s in eb_writeretval\n",
      RunNumber.c_str());
  }

  myconn.disconnect();
}


static void *handle(void *ptr) // This defines a thread to decode files
{
  long int usb = (long int) ptr; // XXX munging a void* into an int!
  while(!OVUSBStream[(int)usb].decode()) usleep(100);
  return NULL;
}

static void *joiner(void *ptr) // This thread joins the above threads
{
  long int nusb = (long int) ptr;
  if((int)nusb < numUSB)
    for(int n=0; n<(int)nusb; n++)
      gThreads[Datamap[n]]->Join();
  else
    for(int n=0; n<numUSB; n++)
      gThreads[n]->Join();
  finished = true;
  return NULL;
}

// opens output data file
static int open_file(string name)
{
  int temp_dataFile = open(name.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
  if ( temp_dataFile < 0 ) {
    log_msg(LOG_CRIT, "Fatal Error: failed to open file %s\n", name.c_str());
    write_ebretval(-1);
    exit(1);
  }
  return temp_dataFile;
}

static int check_disk_space(string dir)
{
  struct statvfs fiData;
  if((statvfs(dir.c_str(), &fiData)) < 0 ) {
    log_msg(LOG_NOTICE, "Error: Failed to stat %s\n", dir.c_str());
    return 0;
  }
  else {
    if(fiData.f_blocks) {
      const int free_space_percent =
        (int)(100*(double)fiData.f_bfree/(double)fiData.f_blocks);
      if(free_space_percent < 3) {
        log_msg(LOG_ERR, "Error: Can't write to disk: Found disk "
          ">97 percent full: statvfs called on %s\n", dir.c_str());
        return -1;
      }
    }
    else {
      log_msg(LOG_ERR, "Error: statvfs failed to read blocks in %s\n", dir.c_str());
      return -1;
    }
  }
  return 0;
}

// FixME: To be optimized (Was never done for Double Chooz.  Is it inefficient?)
static void check_status()
{
  static int Ddelay = 0;
  // Performance monitor
  cout << "Found " << files.size() << " files." << endl;
  int f_delay = (int)(latency*files.size()/numUSB/20);
  if(f_delay != OV_EB_State) {
    if(f_delay > OV_EB_State) {
      if(OV_EB_State <= initial_delay) { // OV EBuilder was not already behind
        log_msg(LOG_NOTICE, "Falling behind processing files\n");
      }
      else { // OV EBuilder was already behind
        Ddelay = f_delay - initial_delay;
        if(Ddelay % 3 == 0) { // Every minute of delay
          Ddelay /= 3;
          log_msg(LOG_NOTICE,
            "Process has accumulated %d min of delay since starting\n", Ddelay);
        }
      }
    }
    else if(f_delay < OV_EB_State) {
      if(OV_EB_State >= initial_delay) {
        log_msg(LOG_NOTICE, "Catching up processing files\n");
      }
      else {
        Ddelay = initial_delay - f_delay;
        if(Ddelay % 3 == 0) { // Every minute of recovery
          Ddelay /= 3;
          log_msg(LOG_NOTICE, "Process has reduced data processing delay by %d "
            "min since starting\n", Ddelay);
        }
      }
    }
    OV_EB_State = f_delay;
  }
}

// Loads files
// Just check that it exists
// (XXX which is it?)
static bool LoadRun()
{
  files.clear();
  if(GetDir(BinaryDir, files)) {
    if(errno) {
      log_msg(LOG_CRIT, "Error(%d) opening dir %s\n", errno, BinaryDir.c_str());
      write_ebretval(-1);
      exit(1);
    }
    return false;
  }
  else {
    initial_delay = (int)(latency*files.size()/numUSB/20);
    OV_EB_State = initial_delay;
  }

  sort(files.begin(), files.end());

  const string TempProcessedOutput = OutputFolder + "/processed";
  umask(0);
  if(mkdir(OutputFolder.c_str(), 0777)) {
    if(EBRunMode != kRecovery) {
      log_msg(LOG_CRIT, "Error creating output file %s\n", OutputFolder.c_str());
      write_ebretval(-1);
      exit(1);
    }
  }
  else if(mkdir(TempProcessedOutput.c_str(), 0777)) {
    if(EBRunMode != kRecovery) {
      log_msg(LOG_CRIT, "Error creating output file %s\n",
        TempProcessedOutput.c_str());
      write_ebretval(-1);
      exit(1);
    }
  }
  else { // FixMe: To optimize (logic) (Was never done for Double Chooz - needed?)
    if(EBRunMode == kRecovery) {
      log_msg(LOG_CRIT, "Output dirs did not already exist in recovery mode.\n");
      write_ebretval(-1);
      exit(1);
    }
  }

  return true;
}

// Loads files
// Checks to see if files are ready to be processed
// (XXX Which is it?)
static int LoadAll()
{
  const int r = check_disk_space(BinaryDir);
  if(r < 0) {
    log_msg(LOG_CRIT, "Fatal error in check_disk_space(%s)\n", BinaryDir.c_str());
    return r;
  }

  if(EBRunMode != kReprocess) {
    // FixME: Is 2*numUSB sufficient to guarantee a match?
    if((int)files.size() <= 3*numUSB) {
      files.clear();
      if(GetDir(BinaryDir, files))
        return 0;
    }
  }
  if((int)files.size() < numUSB)
    return 0;

  sort(files.begin(), files.end()); // FixME: Is it safe to avoid this every time?

  vector<string>::iterator fname_begin=files.begin();

  check_status(); // Performance monitor

  string fdelim = "_"; // Assume files are of form xxxxxxxxx_xx
  size_t fname_it_delim;
  string ftime_min;
  if(fname_begin->find(fdelim) == fname_begin->npos) {
    log_msg(LOG_CRIT, "Fatal Error: Cannot find '_' in input file name\n");
    return -1;
  }

  fname_it_delim = fname_begin->find(fdelim);
  string temp3;
  string fusb;
  for(int k = 0; k<numUSB; k++) {
    for(int j = k; j<(int)files.size(); j++) {
      fusb = (files[j]).substr(fname_it_delim+1, (files[j]).npos);
      if(strtol(fusb.c_str(), NULL, 10) == OVUSBStream[k].GetUSB()) {
        temp3 = files[j];
        files[j] = files[k];
        files[k] = temp3;
        break;
      }
      if(j==(int)(files.size()-1)) { // Failed to find a file for USB k
        log_msg(LOG_WARNING, "USB %d data file not found\n",
          OVUSBStream[k].GetUSB());
        files.clear();
        return 0;
      }
    }
  }

  fname_begin = files.begin();

  int status = 0;
  string base_filename;
  for(int k=0; k<numUSB; k++) {
    fname_it_delim = fname_begin->find(fdelim);
    ftime_min = fname_begin->substr(0, fname_it_delim);
    fusb = fname_begin->substr(fname_it_delim+1, fname_begin->npos);
    // Error: All usbs should have been assigned by MySQL
    if(OVUSBStream[k].GetUSB() == -1) {
      log_msg(LOG_CRIT, "Fatal Error: USB number unassigned\n");
      return -1;
    }
    else { // Check to see that USB numbers are aligned
      if(OVUSBStream[k].GetUSB() != strtol(fusb.c_str(), NULL, 10)) {
        log_msg(LOG_CRIT, "Fatal Error: USB number misalignment\n");
        return -1;
      }
    }
    // Build input filename ( _$usb will be added by LoadFile function )
    base_filename=BinaryDir;
    base_filename.append("/");
    base_filename.append(ftime_min);
    if( (status = OVUSBStream[k].LoadFile(base_filename)) < 1 ) // Can't load file
      return status;

    fname_begin++; // Increment file name iterator
  }

  files.assign(fname_begin, files.end());
  return 1;
}

static void BuildEvent(DataVector *OutDataVector,
                       vector<int> *OutIndexVector, int mydataFile)
{

  int k, nbs, length, module, type, nwords, module_local, usb;
  char channel;
  short int charge;
  long int time_s_hi = 0;
  long int time_s_lo = 0;
  long int time_16ns_hi = 0;
  long int time_16ns_lo = 0;
  DataVector::iterator CurrentOutDataVectorIt = OutDataVector->begin();
  vector<int>::iterator CurrentOutIndexVectorIt = OutIndexVector->begin();
  OVEventHeader *CurrEventHeader = new OVEventHeader;
  OVDataPacketHeader *CurrDataPacketHeader = new OVDataPacketHeader;

  int cnt=0;

  if(mydataFile <= 0) {
    log_msg(LOG_CRIT, "Fatal Error in Build Event. Invalid file "
      "handle for previously opened data file!\n");
    write_ebretval(-1);
    exit(1);
  }

  while( CurrentOutDataVectorIt != OutDataVector->end() ) {

    if(CurrentOutDataVectorIt->size() < 7) {
      log_msg(LOG_CRIT, "Fatal Error in Build Event: Vector of "
        "data found with too few words.\n");
      write_ebretval(-1);
      exit(1);
    }

    // First packet in built event
    if(CurrentOutDataVectorIt == OutDataVector->begin()) {
      time_s_hi = (CurrentOutDataVectorIt->at(1) << 8) + CurrentOutDataVectorIt->at(2);
      time_s_lo = (CurrentOutDataVectorIt->at(3) << 8) + CurrentOutDataVectorIt->at(4);
      CurrEventHeader->SetTimeSec(time_s_hi*0x10000 + time_s_lo);
      CurrEventHeader->SetNOVDataPackets(OutDataVector->size());

      nbs = write(mydataFile, CurrEventHeader, sizeof(OVEventHeader));

      if (nbs<0){
        log_msg(LOG_CRIT, "Fatal Error: Cannot write event header to disk!\n");
        write_ebretval(-1);
        exit(1);
      } // To be optimized
    }

    nwords = (CurrentOutDataVectorIt->at(0) & 0xff) - 1;
    module_local = (CurrentOutDataVectorIt->at(0) >> 8) & 0x7f;
    // EBuilder temporary internal mapping is decoded back to pmtbaord_u from
    // MySQL table
    usb = OVUSBStream[*CurrentOutIndexVectorIt].GetUSB();
    module = PMTUniqueMap[usb*1000+module_local];
    type = CurrentOutDataVectorIt->at(0) >> 15;
    time_16ns_hi = CurrentOutDataVectorIt->at(5);
    time_16ns_lo = CurrentOutDataVectorIt->at(6);

    // Sync Pulse Diagnostic Info: Sync pulse expected at clk count =
    // 2^(SYNC_PULSE_CLK_COUNT_PERIOD_LOG2).  Look for overflows in 62.5 MHz
    // clock count bit corresponding to SYNC_PULSE_CLK_COUNT_PERIOD_LOG2
    if( (time_16ns_hi >> (SYNC_PULSE_CLK_COUNT_PERIOD_LOG2 - 16)) ) {
      if(!overflow[module]) {
        log_msg(LOG_WARNING, "Module %d missed sync pulse near "
          "time stamp %ld\n", module, time_s_hi*0x10000+time_s_lo);
        overflow[module] = true;
        maxcount_16ns_lo[module] = time_16ns_lo;
        maxcount_16ns_hi[module] = time_16ns_hi;
      }
      else {
        maxcount_16ns_lo[module] = time_16ns_lo;
        maxcount_16ns_hi[module] = time_16ns_hi;
      }
    }
    else {
      if(overflow[module]) {
        log_msg(LOG_WARNING, "Module %d max clock count hi: %ld\tlo: %ld\n",
          module, maxcount_16ns_hi[module], maxcount_16ns_lo[module]);
        maxcount_16ns_lo[module] = time_16ns_lo;
        maxcount_16ns_hi[module] = time_16ns_hi;
        overflow[module] = false;
      }
    }

    // For latch packets, compute the number of hits
    k = 0;
    if(type == 0) {
      for(int w = 0; w < nwords - 3 ; w++) { // fan-in packets have length=6
        int temp = CurrentOutDataVectorIt->at(7+w) + 0;
        for(int n = 0; n < 16; n++) {
          if(temp & 1) k++;
          temp >>= 1;
        }
      }
      length = k;
      if(nwords == 5) { // trigger box packet
        type = 2; // Set trigger box packet type

        /* Sync Pulse Diagnostic Info: Throw error if sync pulse does not come
         * at expected clock count */
        // Firmware only allows this for special sync pulse packets in trigger box
        if(length == 32) {
          const long int time_16ns_sync = time_16ns_hi*0x10000+time_16ns_lo;
          const long int expected_time_16ns_sync =
            (1 << SYNC_PULSE_CLK_COUNT_PERIOD_LOG2) - 1;
          const long int expected_time_16ns_sync_offset =
            *(PMTOffsets[usb]+module_local);

          if(expected_time_16ns_sync - expected_time_16ns_sync_offset
             != time_16ns_sync)
            log_msg(LOG_ERR, "Trigger box module %d received sync pulse at "
              "clock count %ld instead of expected clock count (%ld).\n",
              module, time_16ns_sync, expected_time_16ns_sync);
        }
      }
    }
    else {
      length = (CurrentOutDataVectorIt->size()-7)/2;
    }

    CurrDataPacketHeader->SetNHits((char)length);
    CurrDataPacketHeader->SetModule((short int)module);
    CurrDataPacketHeader->SetType((char)type);
    CurrDataPacketHeader->SetTime16ns(time_16ns_hi*0x10000 + time_16ns_lo);

    nbs = write(mydataFile, CurrDataPacketHeader, sizeof(OVDataPacketHeader));
    if (nbs<0){
      log_msg(LOG_CRIT, "Fatal Error: Cannot write data packet header to disk!\n");
      write_ebretval(-1);
      exit(1);
    } // To be optimize -- never done for Double Chooz.  Needed?

    if(type == 1) { // PMTBOARD ADC Packet

      for(int m = 0; m <= length - 1; m++) { //Loop over all hits in the packet

        channel = (char)CurrentOutDataVectorIt->at(8+2*m);
        charge = (short int)CurrentOutDataVectorIt->at(7+2*m);
        OVHitData CurrHit;
        CurrHit.SetHit(channel, charge);

        nbs = write(mydataFile, &CurrHit, sizeof(OVHitData));
        if (nbs<0){
          log_msg(LOG_CRIT, "Fatal Error: Cannot write hit to disk!\n");
          write_ebretval(-1);
          exit(1);
        } // To be optimized -- never done for Double Chooz.  Needed?

        cnt++;
      }
    }
    else { // PMTBOARD LATCH PACKET OR TRIGGER BOX PACKET

      for(int w = 0; w < nwords-3 ; w++) {
        int temp = CurrentOutDataVectorIt->at(7+w) + 0;
        for(int n = 0; n < 16; n++) {
          if(temp & 1) {
            OVHitData CurrHit;
            CurrHit.SetHit(16*(nwords-4-w) + n, 1);

            nbs = write(mydataFile, &CurrHit, sizeof(OVHitData));
            if (nbs<0){
              log_msg(LOG_CRIT, "Fatal Error: Cannot write hit to disk!\n");
              write_ebretval(-1);
              exit(1);
            } // To be optimized -- never done for Double Chooz.  Needed?

            cnt++;
          }
          temp >>=1;
        }
      }
    }
    CurrentOutDataVectorIt++;
    CurrentOutIndexVectorIt++;
  } // For all events in data packet

  delete CurrEventHeader;
  delete CurrDataPacketHeader;
}

static bool parse_options(int argc, char **argv)
{
  if(argc <= 1) goto fail;

  char c;
  while ((c = getopt (argc, argv, "r:t:T:R:H:e:h:")) != -1) {
    char buf[BUFSIZE];

    switch (c) {
    // XXX no overflow protection
    case 'r': strcpy(buf, optarg); RunNumber = buf; break;
    case 'H': strcpy(buf, optarg); OVDAQHost = buf; break;
    case 'R': strcpy(buf, optarg); OVRunType = buf;  break;

    case 't': Threshold = atoi(optarg); break;
    case 'T': EBTrigMode = (TriggerMode)atoi(optarg); break;
    case 'e': OutDisk = atoi(optarg); break;
    case 'h':
    default:  goto fail;
    }
  }
  if(optind < argc){
    printf("Unknown options given\n");
    goto fail;
  }
  if(EBTrigMode < kNone || EBTrigMode > kDoubleLayer){
    printf("Invalid trigger mode %d\n", EBTrigMode);
    goto fail;
  }
  if(Threshold < 0) {
    printf("Negative thresholds not allowed.\n");
    goto fail;
  }

  for(int index = optind; index < argc; index++){
    printf("Non-option argument %s\n", argv[index]);
    goto fail;
  }

  return true;

  fail:
  printf("Usage: %s -r <run_number> [-d <data_disk>]\n"
         "      [-t <offline_threshold>] [-T <offline_trigger_mode>] [-R <run_type>]\n"
         "      [-H <OV_DAQ_data_mount_point>] [-e <EBuilder_output_disk>]\n"
         "-r : expected run # for incoming data\n"
         "     [default: Run_yyyymmdd_hh_mm (most recent)]\n"
         "-t : offline threshold (ADC counts) to apply\n"
         "     [default: 0 (no software threshold)]\n"
         "-T : offline trigger mode (0: NONE, 1: OR, 2: AND)\n"
         "     [default: 0 (No trigger pattern between layers)]\n"
         "-R : OV run type (P: physics, C: calib, D: debug)\n"
         "     [default: P]\n"
         "-H : OV DAQ mount path on EBuilder machine [default: ovfovdaq]\n"
         "-e : disk number of OV EBuilder output binary [default: 1]\n",
         argv[0]);
  return false;
}

static void CalculatePedestal(DataVector* BaselineData, int **baseptr)
{
  double baseline[maxModules][numChannels] = {};
  int counter[maxModules][numChannels] = {};
  int channel, charge, module, type;
  channel = charge = module = 0;

  for(DataVector::iterator BaselineDataIt = BaselineData->begin();
      BaselineDataIt != BaselineData->end();
      BaselineDataIt++) {

    // Data Packets should have 7 + 2*num_hits elements
    if(BaselineDataIt->size() < 8 && BaselineDataIt->size() % 2 != 1)
      log_msg(LOG_ERR, "Fatal Error: Baseline data packet found with no data\n");

    module = (BaselineDataIt->at(0) >> 8) & 0x7f;
    type = BaselineDataIt->at(0) >> 15;

    if(type) {
      if(module > maxModules)
        log_msg(LOG_ERR, "Fatal Error: Module number requested "
          "(%d) out of range in calculate pedestal\n", module);

      for(int i = 7; i+1 < (int)BaselineDataIt->size(); i=i+2) {
        charge = BaselineDataIt->at(i);
        channel = BaselineDataIt->at(i+1); // Channels run 0-63
        if(channel >= numChannels)
          log_msg(LOG_ERR, "Fatal Error: Channel number requested "
            "(%d) out of range in calculate pedestal\n", channel);
        // Should these be modified to better handle large numbers of baseline
        // triggers?
        baseline[module][channel] = (baseline[module][channel]*
          counter[module][channel] + charge)/(counter[module][channel]+1);
        counter[module][channel] = counter[module][channel] + 1;
      }
    }
  }

  for(int i = 0; i < maxModules; i++)
    for( int j = 0; j < numChannels; j++)
      *(*(baseptr+i) + j) = (int)baseline[i][j];
}

static bool WriteBaselineTable(int **baseptr, int usb)
{
  return true; // XXX No database, so just pretend things are ok

  mysqlpp::Connection myconn(false); // false to not throw exceptions on errors
  mysqlpp::StoreQueryResult res;

  if(!myconn.connect(database, server, username, password)) {
    log_msg(LOG_NOTICE, "Cannot connect to MySQL database %s at %s\n",
      database, server);
    return false;
  }

  char query_string[BUFSIZE];

  time_t rawtime;
  struct tm * timeinfo;

  time ( &rawtime );
  timeinfo = localtime ( &rawtime );

  char mydate[BUFSIZE];
  char mytime[BUFSIZE];
  char mybuff[BUFSIZE];
  sprintf(mydate, "%.4d-%.2d-%.2d", 1900+timeinfo->tm_year,
          1+timeinfo->tm_mon, timeinfo->tm_mday);
  sprintf(mytime, "%.2d:%.2d:%.2d", timeinfo->tm_hour, timeinfo->tm_min,
    timeinfo->tm_sec);
  string baseline_values = "";

  for(int i = 0; i < maxModules; i++) {
    baseline_values = "";
    for( int j = 0; j < numChannels; j++) {
      if( *(*(baseptr+i) + j) > 0) {

        if(j==0) {
          sprintf(mybuff, ",%d", PMTUniqueMap[1000*usb+i]);
          baseline_values.append(mybuff); // Insert board_address
        }

        sprintf(mybuff, ",%d", *(*(baseptr+i) + j)); // Insert baseline value
        baseline_values.append(mybuff);

        if(j==numChannels-1) { // last baseline value has been inserted

          sprintf(query_string, "INSERT INTO OV_pedestal "
            "VALUES ('%s','%s','%s',''%s);",
            RunNumber.c_str(), mydate, mytime, baseline_values.c_str());

          mysqlpp::Query query = myconn.query(query_string);
          if(!query.execute()) {
            log_msg(LOG_NOTICE, "MySQL query (%s) error: %s\n",
                    query_string, query.error());
            myconn.disconnect();
            return false;
          }
        }
      }
    }
  }

  myconn.disconnect();
  return true;
}

static bool GetBaselines()
{
  // Search baseline directory for files and sort them lexigraphically
  vector<string> in_files_tmp;
  if(GetDir(BinaryDir, in_files_tmp, false, true)) { // Get baselines too
    if(errno)
      log_msg(LOG_ERR, "Fatal Error(%d) opening binary "
        "directory %s for baselines\n", errno, BinaryDir.c_str());
    return false;
  }
  else {
    printf("Processing baselines...\n");
  }

  // Preparing for baseline shift
  vector<string> in_files;
  for(vector<string>::iterator file = in_files_tmp.begin();
      file != in_files_tmp.end(); file++)
    if(file->find("baseline") != file->npos)
      in_files.push_back(*file);

  sort(in_files.begin(), in_files.end());

  // Sanity check on number of baseline files
  if((int)in_files.size() != num_nonFanUSB) {
    log_msg(LOG_ERR, "Fatal Error: Baseline file count (%lu) != "
      "numUSB (%d) in directory %s\n", (long int)in_files.size(), numUSB,
       BinaryDir.c_str());
    return false;
  }

  // Set USB numbers for each OVUSBStream and load baseline files
  for(int i = 0; i < num_nonFanUSB; i++) {
    // Error: all usbs should have been assigned from MySQL
    if( OVUSBStream[Datamap[i]].GetUSB() == -1 ) {
      log_msg(LOG_ERR, "Fatal Error: Found USB number "
        "unassigned while getting baselines\n");
      return false;
    }
    if(OVUSBStream[Datamap[i]].LoadFile(BinaryDir+ "/baseline") < 1)
      return false; // Load baseline file for data streams
  }

  // Decode all files at once and load into memory
  for(int j=0; j<num_nonFanUSB; j++) { // Load all files in at once
    printf("Starting Thread %d\n", Datamap[j]);
    gThreads[Datamap[j]] =
      new TThread(Form("gThreads%d", Datamap[j]), handle, (void*) Datamap[j]);
    gThreads[Datamap[j]]->Run();
  }

  // XXX what's the point of running a thread, then blocking until it finishes?
  // Why not just call the function?
  //                                  XXX munging an int into a void*!
  joinerThread = new TThread("joinerThread", joiner, (void*)num_nonFanUSB);
  joinerThread->Run();

  while(!finished) sleep(1); // To be optimized -- never done for Double Chooz - needed?
  finished = false;

  joinerThread->Join();

  for(int k=0; k < num_nonFanUSB; k++) delete gThreads[Datamap[k]];
  delete joinerThread;

  cout << "Joined all threads!\n";

  // Build baseline tables
  // XXX I think this can be replaced with a zeroed 2D array in one line
  int **baselines = new int*[maxModules];
  for(int i = 0; i<maxModules; i++) {
    baselines[i] = new int[numChannels];
    for(int j = 0; j<numChannels; j++)
      *(*(baselines+i)+j) = 0;
  }

  for(int i = 0; i < num_nonFanUSB; i++) {
    DataVector BaselineData;
    OVUSBStream[Datamap[i]].GetBaselineData(&BaselineData);
    CalculatePedestal(&BaselineData, baselines);
    OVUSBStream[Datamap[i]].SetBaseline(baselines); // Should check for success here?
    const int usb = OVUSBStream[Datamap[i]].GetUSB(); // Should I check for success here?
    if(!WriteBaselineTable(baselines, usb)) {
      log_msg(LOG_ERR, "Fatal Error writing baseline table to MySQL database\n");
      return false;
    }
  }

  for(int i = 0; i<maxModules; i++)
    delete [] baselines[i];
  delete [] baselines;

  return true;
}


static bool write_ebsummary()
{
  return true; // XXX no database, so just pretend this worked.

  mysqlpp::Connection myconn(false); // false to not throw exceptions on errors
  mysqlpp::StoreQueryResult res;

  if(!myconn.connect(database, server, username, password)) {
    log_msg(LOG_WARNING, "Cannot connect to MySQL database %s at %s\n",
      database, server);
    return false;
  }

  char query_string[BUFSIZE];

  sprintf(query_string, "SELECT Path FROM OV_ebuilder "
    "WHERE Run_number = '%s', SW_Threshold = '%04d', "
    "SW_TriggerMode = '%01d', Res1 = '00', Res2 = '00';",
    RunNumber.c_str(), Threshold, (int)EBTrigMode);
  mysqlpp::Query query = myconn.query(query_string);
  res = query.store();

  if(res.num_rows() == 0){ // Run has never been reprocessed with this configuration
    sprintf(query_string, "INSERT INTO OV_ebuilder "
      "(Run_number,Path,SW_Threshold,SW_TriggerMode,Res1,Res2) "
      "VALUES ('%s','%s','%04d','%01d','00','00');",
      RunNumber.c_str(), OutputFolder.c_str(), Threshold, (int)EBTrigMode);

    mysqlpp::Query query2 = myconn.query(query_string);
    if(!query2.execute()) {
      log_msg(LOG_WARNING, "MySQL query (%s) error: %s\n",
        query_string, query2.error());
      myconn.disconnect();
      return false;
    }
  }

  myconn.disconnect();
  return true;
}

static void die_with_log(const char * const format, ...)
{
  va_list ap;
  va_start(ap, format);
  log_msg(LOG_CRIT, format, ap);
  write_ebretval(-1);
  exit(127);
}

// Like sprintf(), but returns a std::string instead of writing into a
// character buffer.  Truncates the string at BUFSIZE.
static string cpp_sprintf(const char * format, ...)
{
  va_list ap;
  va_start(ap, format);
  char buf[BUFSIZE];
  vsnprintf(buf, BUFSIZE, format, ap);
  return buf;
}

// Return the name of the config table for this run.  Sets no globals.
static char * get_config_table_name(mysqlpp::Connection * myconn)
{
  if(myconn == NULL) return NULL;

  char query_string[BUFSIZE];
  sprintf(query_string, "SELECT config_table FROM OV_runsummary "
    "WHERE Run_number = '%s' ORDER BY start_time DESC;", RunNumber.c_str());
  const mysqlpp::StoreQueryResult res = myconn->query(query_string).store();
  if(res.num_rows() < 1)
    die_with_log("Found no matching entry for run %s in OV_runsummary\n",
      RunNumber.c_str());
  else if(res.num_rows() > 1)
    // Really using the most recent entry?  I think res[0] probably means
    // the oldest entry, but I'm not sure.
    log_msg(LOG_WARNING, "Found more than one entry for run %s "
      "in OV_runsummary. Using most recent entry.\n", RunNumber.c_str());
  else
    log_msg(LOG_INFO, "Found MySQL run summary entry for run: %s\n",
      RunNumber.c_str());

  static char config_table[BUFSIZE];
  strcpy(config_table, res[0][0].c_str());
  return config_table;
}

// Return a list of distict USB serial numbers for the given config table.
// If positiveHV is true, then require the HV column to be not -999, which
// marks the USB as a Fan-In, whatever that is.  Sets no globals.
static vector<int> get_distinct_usb_serials(mysqlpp::Connection * myconn,
                                            const char * const config_table,
                                            const bool positiveHV)
{
  vector<int> serials;
  if(myconn == NULL){
    serials.push_back(5);
    serials.push_back(15);
    serials.push_back(16);
    serials.push_back(17);
    serials.push_back(18);
    if(!positiveHV) serials.push_back(28); // the only fan-in
    return serials;
  }

  char query_string[BUFSIZE];
  sprintf(query_string, "SELECT DISTINCT USB_serial FROM %s %s ORDER BY USB_serial;",
    config_table, positiveHV?"WHERE HV != -999":"");
  mysqlpp::Query query = myconn->query(query_string);
  mysqlpp::StoreQueryResult res = query.store();
  if(res.num_rows() < 1)
    die_with_log("MySQL query (%s) error: %s\n", query_string, query.error());
  log_msg(LOG_INFO, "Found %u distinct %sUSBs in table %s\n",
          res.num_rows(), positiveHV?"non-Fan-in ":"", config_table);
  for(unsigned int i = 0; i < res.num_rows(); i++)
    serials.push_back(res[i][0]);
  return serials;
}

// Return a vector of {USB serial numbers, board numbers, time offsets, pmtboard_u}
// for all USBs in the given table.  Sets no globals.
static vector<usb_sbop> get_sbops(mysqlpp::Connection * myconn,
                                  const char * const config_table)
{
  vector<usb_sbop> sbops;
  if(myconn == NULL){
    sbops.push_back(usb_sbop(15,0,200,0));
    sbops.push_back(usb_sbop(15,2,202,0));
    sbops.push_back(usb_sbop(15,3,203,0));
    sbops.push_back(usb_sbop(15,4,204,0));
    sbops.push_back(usb_sbop(15,5,205,0));
    sbops.push_back(usb_sbop(15,6,206,0));
    sbops.push_back(usb_sbop(15,7,207,0));
    sbops.push_back(usb_sbop(15,8,208,0));
    sbops.push_back(usb_sbop(15,9,209,0));
    sbops.push_back(usb_sbop(15,10,210,0));
    sbops.push_back(usb_sbop(16,11,211,0));
    sbops.push_back(usb_sbop(16,12,212,0));
    sbops.push_back(usb_sbop(16,13,213,0));
    sbops.push_back(usb_sbop(16,14,214,0));
    sbops.push_back(usb_sbop(16,15,215,0));
    sbops.push_back(usb_sbop(16,16,216,0));
    sbops.push_back(usb_sbop(16,17,217,0));
    sbops.push_back(usb_sbop(16,18,218,0));
    sbops.push_back(usb_sbop(16,19,219,0));
    sbops.push_back(usb_sbop(16,20,220,0));
    sbops.push_back(usb_sbop(5,21,221,0));
    sbops.push_back(usb_sbop(5,22,222,0));
    sbops.push_back(usb_sbop(5,23,223,0));
    sbops.push_back(usb_sbop(5,24,224,0));
    sbops.push_back(usb_sbop(5,25,225,0));
    sbops.push_back(usb_sbop(5,26,226,0));
    sbops.push_back(usb_sbop(5,27,227,0));
    sbops.push_back(usb_sbop(5,28,228,0));
    sbops.push_back(usb_sbop(5,29,229,0));
    sbops.push_back(usb_sbop(5,30,230,0));
    sbops.push_back(usb_sbop(17,31,231,0));
    sbops.push_back(usb_sbop(17,32,232,0));
    sbops.push_back(usb_sbop(17,33,233,0));
    sbops.push_back(usb_sbop(17,34,234,0));
    sbops.push_back(usb_sbop(17,35,235,0));
    sbops.push_back(usb_sbop(17,36,236,0));
    sbops.push_back(usb_sbop(17,37,237,0));
    sbops.push_back(usb_sbop(17,38,238,0));
    sbops.push_back(usb_sbop(17,39,239,0));
    sbops.push_back(usb_sbop(17,40,240,0));
    sbops.push_back(usb_sbop(18,41,241,0));
    sbops.push_back(usb_sbop(18,42,242,0));
    sbops.push_back(usb_sbop(18,43,243,0));
    sbops.push_back(usb_sbop(18,44,244,0));
    sbops.push_back(usb_sbop(18,45,245,0));
    sbops.push_back(usb_sbop(18,46,246,0));
    sbops.push_back(usb_sbop(18,47,247,0));
    sbops.push_back(usb_sbop(18,48,248,0));
    sbops.push_back(usb_sbop(18,49,249,0));
    sbops.push_back(usb_sbop(18,50,250,0));
    sbops.push_back(usb_sbop(18,51,251,0));
    sbops.push_back(usb_sbop(18,52,252,0));
    sbops.push_back(usb_sbop(18,53,253,0));
    sbops.push_back(usb_sbop(18,54,254,0));
    sbops.push_back(usb_sbop(18,55,255,0));
    sbops.push_back(usb_sbop(18,56,256,0));
    sbops.push_back(usb_sbop(28,58,258,0));
    sbops.push_back(usb_sbop(28,59,259,0));
    sbops.push_back(usb_sbop(28,60,260,0));
    sbops.push_back(usb_sbop(28,61,261,0));
    return sbops;
  }

  char query_string[BUFSIZE];
  sprintf(query_string, "SELECT USB_serial, board_number, offset, pmtboard_u FROM %s;",
    config_table);
  mysqlpp::Query query = myconn->query(query_string);
  mysqlpp::StoreQueryResult res=query.store();
  if(res.num_rows() < 1)
    die_with_log("MySQL query (%s) error: %s\n", query_string, query.error());
  log_msg(LOG_INFO, "Found time offsets for online table %s\n", config_table);

  for(unsigned int i = 0; i < res.num_rows(); i++)
    sbops.push_back(usb_sbop(atoi(res[i][0]), atoi(res[i][1]),
                             atoi(res[i][2]), atoi(res[i][3])));
  return sbops;
}

static std::pair<int, int> board_count(mysqlpp::Connection * myconn,
                                       const char * const config_table)
{
  if(myconn == NULL)
    return std::pair<int, int>(60, 261); // from online400_no_ts7

  char query_string[BUFSIZE];
  sprintf(query_string, "SELECT DISTINCT pmtboard_u FROM %s;", config_table);
  mysqlpp::Query query = myconn->query(query_string);
  mysqlpp::StoreQueryResult res = query.store();
  if(res.num_rows() < 1)
    die_with_log("MySQL query (%s) error: %s\n", query_string, query.error());
  log_msg(LOG_INFO, "Found %u distinct PMT boards in table %s\n",
          res.num_rows(), config_table);

  int max = 0;
  for(unsigned int i = 0; i < res.num_rows(); i++) {
    const int pmtnum = atoi(res[i][0]);
    if(pmtnum > max) max = pmtnum;
  }
  log_msg(LOG_INFO, "Found max PMT board number %d in table %s\n", max, config_table);

  return std::pair<int, int>(res.num_rows(), max);
}

// Gets the necessary run info from the run summary table, does some checking,
// and returns the values that will be used to set globals.  No globals set here.
static some_run_info get_some_run_info(mysqlpp::Connection * myconn)
{
  if(myconn == NULL){
    some_run_info info;
    memset(&info, 0, sizeof(some_run_info));
    info.has_ebcomment = false;
    info.has_ebsubrun = false;
    info.has_stoptime = true;
    info.daqdisk = 2;
    return info;
  }

  char query_string[BUFSIZE];
  sprintf(query_string, "SELECT Run_Type,SW_Threshold,"
    "SW_TriggerMode,daq_disk,EBcomment,EBsubrunnumber,stop_time"
    "FROM OV_runsummary where Run_number = '%s' ORDER BY start_time DESC;",
    RunNumber.c_str());
  mysqlpp::Query query = myconn->query(query_string);
  mysqlpp::StoreQueryResult res = query.store();
  if(res.num_rows() < 1)
    die_with_log("MySQL query (%s) error: %s\n", query_string, query.error());
  if(res.num_rows() > 1) // Check that OVRunType is the same
    log_msg(LOG_WARNING, "Found more than one entry for run %s in "
      "OV_runsummary. Using most recent entry\n", RunNumber.c_str());
  else
    log_msg(LOG_INFO, "Found MySQL run summary entry for run: %s\n",
            RunNumber.c_str());

  if(OVRunType != res[0][0].c_str())
    die_with_log("MySQL Run Type: %s does not match command line "
      "Run Type: %s\n", res[0][0].c_str(), OVRunType.c_str());

  // Sanity check for each run mode
  if(EBRunMode == kReprocess &&
     atoi(res[0][1]) == Threshold && (TriggerMode)atoi(res[0][2]) == EBTrigMode)
    die_with_log("MySQL running parameters match "
      "reprocessing parameters. Threshold: %04d\tTrigger type: %01d\n",
      Threshold, (int)EBTrigMode);

  if(EBRunMode == kRecovery &&
    (res[0][0].c_str() != OVRunType ||
       atoi(res[0][1]) != Threshold ||
       atoi(res[0][2]) != (int)EBTrigMode))
    die_with_log("MySQL parameters do not match recovery "
      "parameters. RunType: %s\tThreshold: %04d\tTrigger type: %01d\n",
      OVRunType.c_str(), Threshold, (int)EBTrigMode);

  if(EBRunMode != kReprocess) {
    // XXX except then it isn't actually changed
    if(EBTrigMode != (TriggerMode)atoi(res[0][2]))
      log_msg(LOG_WARNING, "Trigger Mode requested (%d) will "
        "override MySQL setting (%d)\n", (int)EBTrigMode, atoi(res[0][2]));

    // XXX except then it isn't actually changed
    if(Threshold != atoi(res[0][1]))
      log_msg(LOG_WARNING, "Threshold requested (%d) will override "
        "MySQL setting (%d)\n", Threshold, atoi(res[0][1]));

    printf("Threshold: %d \t EBTrigMode: %d\n", Threshold, EBTrigMode);
  }

  some_run_info info;
  info.daqdisk      = atoi(res[0][3]);
  if((info.has_ebcomment = !res[0][4].is_null())) info.ebcomment = atoi(res[0][4]);
  if((info.has_ebsubrun  = !res[0][5].is_null())) info.ebsubrun  = atoi(res[0][5]);
  info.has_stoptime     = !res[0][6].is_null();

  if(info.daqdisk != 1 && info.daqdisk != 2)
    die_with_log("MySQL query error: could not retrieve "
      "data disk for run: %s\n", RunNumber.c_str());

  return info;
}

// Returns the number of times the run has been processed before
static unsigned int get_ntimesprocessed(mysqlpp::Connection * myconn)
{
  if(myconn == NULL) return 0;

  char query_string[BUFSIZE];
  sprintf(query_string, "SELECT Path FROM OV_ebuilder WHERE Run_number = '%s';",
          RunNumber.c_str());
  mysqlpp::StoreQueryResult res = myconn->query(query_string).store();
  return res.num_rows();
}

// Returns the directory where this run was processed the first time in this
// configuration, or the empty string if it never has been
static string get_same_config_path(mysqlpp::Connection * myconn)
{
  if(myconn == NULL) return "";

  char query_string[BUFSIZE];
  sprintf(query_string, "SELECT Path FROM OV_ebuilder "
    "WHERE Run_number = '%s' and SW_Threshold = '%04d' and "
    "SW_TriggerMode = '%01d' and Res1 = '00' and Res2 = '00' "
    "ORDER BY Entry;", RunNumber.c_str(), Threshold, (int)EBTrigMode);

  mysqlpp::StoreQueryResult res = myconn->query(query_string).store();

  if(res.num_rows() == 0) return "";
  else return (string)res[0][0];
}

// Connects to the database and return a pointer to it.  Or, XXX, actually at
// the moment does not because I don't have one and am not yet sure if I want
// one, so return NULL to indicate that we don't have one.
static mysqlpp::Connection * establish_mysql_connection_or_not()
{
  return NULL;

  // false to not throw exceptions on errors
  mysqlpp::Connection * myconn = new mysqlpp::Connection(false);

  char DCDatabase_path[BUFSIZE];

  // XXX we don't have these environment variables.
  sprintf(DCDatabase_path, "%s/config/DCDatabase.config", getenv("DCONLINE_PATH"));

  sprintf(server, "%s", config_string(DCDatabase_path, "DCDB_SERVER_HOST"));
  sprintf(username, "%s", config_string(DCDatabase_path, "DCDB_OV_USER"));
  sprintf(password, "%s", config_string(DCDatabase_path, "DCDB_OV_PASSWORD"));
  sprintf(database, "%s", config_string(DCDatabase_path, "DCDB_OV_DBNAME"));

  if(!myconn->connect(database, server, username, password))
    die_with_log("Cannot connect to MySQL database %s at %s\n",
      database, server);
  return myconn;
}

// Database tables read from: OV_runsummary; whatever table OV_runsummary names
// in config_table, such as online400; and OV_ebuilder.
static void read_summary_table()
{
  mysqlpp::Connection * myconn = establish_mysql_connection_or_not();

  const char * const config_table = get_config_table_name(myconn);

  const vector<int>
    usbserials       = get_distinct_usb_serials(myconn, config_table, false),
    nonfanin_serials = get_distinct_usb_serials(myconn, config_table, true);
  numUSB        = usbserials.size();
  num_nonFanUSB = nonfanin_serials.size();

  map<int, int> usbmap; // Maps USB number to numerical ordering of all USBs
  for(unsigned int i = 0; i < usbserials.size(); i++) {
    usbmap[usbserials[i]] = i;
    OVUSBStream[i].SetUSB(usbserials[i]);

    // Identify the fan-in USBs. I think no code cares, though.
    if(find(nonfanin_serials.begin(), nonfanin_serials.end(), usbserials[i])
       == nonfanin_serials.end())
      OVUSBStream[i].SetIsFanUSB();
  }

  for(unsigned int i = 0; i < nonfanin_serials.size(); i++)
    Datamap[i] = usbmap[nonfanin_serials[i]];

  // Load the time offsets for these boards
  const vector<usb_sbop> sbops = get_sbops(myconn, config_table);

  // Create map of UBS_serial to array of pmt board offsets
  for(unsigned int i = 0; i < sbops.size(); i++) {
    if(!PMTOffsets.count(sbops[i].serial))
      PMTOffsets[sbops[i].serial] = new int[maxModules];
    if(sbops[i].board < maxModules)
      PMTOffsets[sbops[i].serial][sbops[i].board] = sbops[i].offset;
  }

  // USB to array of PMT offsets
  for(map<int, int*>::iterator os = PMTOffsets.begin(); os != PMTOffsets.end(); os++)
    OVUSBStream[usbmap[os->first]].SetOffset(os->second);

  // Count the number of boards in this setup
  const int totalboards = board_count(myconn, config_table).first;
  const int max_board   = board_count(myconn, config_table).second;
  overflow = new bool[max_board+1];
  maxcount_16ns_hi = new long int[max_board+1];
  maxcount_16ns_lo = new long int[max_board+1];
  memset(overflow, 0, (max_board+1)*sizeof(bool));
  memset(maxcount_16ns_hi, 0, (max_board+1)*sizeof(long int));
  memset(maxcount_16ns_lo, 0, (max_board+1)*sizeof(long int));

  // Count the number of PMT and Fan-in boards in this setup
  if((int)sbops.size() != totalboards) // config table has duplicate entries
    die_with_log("Found duplicate pmtboard_u entries in table %s\n",
            config_table);

  // This is a temporary internal mapping used only by the EBuilder
  for(unsigned int i = 0; i < sbops.size(); i++)
    PMTUniqueMap[1000*sbops[i].serial+sbops[i].board] = sbops[i].pmtboard_u;

  //////////////////////////////////////////////////////////////////////
  // Get run summary information
  const some_run_info runinfo = get_some_run_info(myconn);

  // Set the Data Folder and Ouput Dir
  const string OutputDir = cpp_sprintf("/data%d/OVDAQ/", OutDisk);
  OutputFolder = OutputDir + "DATA/";

  // Assign output folder based on disk number
  const string datadir =
    cpp_sprintf("/%s/data%d/%s", OVDAQHost.c_str(), runinfo.daqdisk, "OVDAQ/DATA");
  BinaryDir = datadir + "/Run_" + RunNumber + "/binary/";
  const string decoded_dir = datadir + "/Run_" + RunNumber + "/decoded/";

  // Determine run mode
  vector<string> initial_files;
  if(runinfo.has_ebcomment) { // EBcomment filled each successful write attempt
    // False if non-baseline files are found
    if(GetDir(BinaryDir, initial_files)) {
      if(errno)
        die_with_log("Error(%d) opening directory %s\n", errno, BinaryDir.c_str());
      if(runinfo.has_stoptime){ // stop_time has been filled and so was a successful run
        EBRunMode = kReprocess;
      }
      else { // stop_time has not been filled. DAQ could be waiting in STARTED_S
        EBRunMode = kRecovery;
        EBcomment = runinfo.ebcomment;
      }
    }
    else {
      EBRunMode = kRecovery;
      EBcomment = runinfo.ebcomment;
    }
  }
  log_msg(LOG_INFO, "OV EBuilder Run Mode: %d\n", EBRunMode);

  if(EBRunMode == kRecovery && runinfo.has_ebsubrun)
    SubRunCounter = timestampsperoutput*runinfo.ebsubrun;

  bool Repeat = false;

  // Figure out run mode, do file handling appropriately
  if(EBRunMode == kReprocess) {
    umask(0);

    const unsigned int ntimesprocessed = get_ntimesprocessed(myconn);

    if(ntimesprocessed == 0) // Can't find run in OV_ebuilder
      die_with_log("EBuilder did not finish processing run %s. "
        "Run recovery mode first.\n", RunNumber.c_str());

    if(ntimesprocessed == 1) { // Run has never been reprocessed
      OutputFolder = cpp_sprintf("%sREP/Run_%s", OutputDir.c_str(), RunNumber.c_str());
      if(mkdir(OutputFolder.c_str(), 0777)) {
        // XXX should use perror here and elsewhere to report all kinds of
        // errors, not just the most common, automatically
        if(errno != EEXIST)
          die_with_log("Error (%d) creating output dir %s\n",
                  errno, OutputFolder.c_str());
        log_msg(LOG_WARNING, "Output dir %s already exists.\n", OutputFolder.c_str());
      }
    }

    const string same_config_path = get_same_config_path(myconn);
    if(myconn != NULL) myconn->disconnect();

    // Run has already been reprocessed with same configuration
    if(same_config_path != "") {
      Repeat = true;
      log_msg(LOG_WARNING, "Same reprocessing configuration found for "
        "run %s at %s. Deleting...\n", RunNumber.c_str(), same_config_path.c_str());

      // Clean up old directory
      vector<string> old_files;
      string tempfile;
      string tempdir = same_config_path + "Run_" + RunNumber + "/processed";
      if(GetDir(tempdir, old_files, true))
        if(errno)
          die_with_log("Error(%d) opening directory %s\n",
                  errno, tempdir.c_str());

      for(int m = 0; m<(int)old_files.size(); m++) {
        tempfile = tempdir + "/" + old_files[m];
        if(remove(tempfile.c_str()))
          die_with_log("Error deleting file %s\n", tempfile.c_str());
      }
      if(rmdir(tempdir.c_str()))
        die_with_log("Error deleting folder %s\n", tempdir.c_str());
      old_files.clear();

      tempdir = same_config_path + "Run_" + RunNumber;
      if(GetDir(tempdir, old_files, true))
        if(errno)
          die_with_log("Error(%d) opening directory %s\n", errno, tempdir.c_str());

      for(int m = 0; m<(int)old_files.size(); m++) {
        tempfile = tempdir + "/" + old_files[m];
        if(remove(tempfile.c_str()))
          die_with_log("Error deleting file %s\n", tempfile.c_str());
      }
      if(rmdir(tempdir.c_str()))
        die_with_log("Error deleting folder %s\n", tempdir.c_str());
    }

    // Create folder based on parameters
    OutputFolder = cpp_sprintf("%sREP/Run_%s/T%dADC%04dP1%02dP2%02d/",
      OutputDir.c_str(), RunNumber.c_str(), (int)EBTrigMode, Threshold, 0, 0);
    if(mkdir(OutputFolder.c_str(), 0777)) {
      if(errno != EEXIST)
        die_with_log("Error (%d) creating output folder %s\n",
                errno, OutputFolder.c_str());
      log_msg(LOG_WARNING, "Output folder %s already exists.\n",
        OutputFolder.c_str());
    }
  }

  if(!Repeat && !write_ebsummary())
    die_with_log("!Repeat && !write_ebsummary\n");

  if(EBRunMode != kNormal) {
    // Move some decoded OV binary files for processing
    initial_files.clear();
    if(GetDir(decoded_dir, initial_files, true)) {
      if(errno)
        die_with_log("Error (%d) opening directory %s\n",
                errno, decoded_dir.c_str());
      die_with_log("No decoded files found in directory %s\n",
        decoded_dir.c_str());
    }
    sort(initial_files.begin(), initial_files.end());

    // Determine files to rename
    vector<string>::iterator fname_begin=initial_files.begin();
    const string fdelim = "_"; // Assume files are of form xxxxxxxxx_xx.done
    if(fname_begin->find(fdelim) == fname_begin->npos)
      die_with_log("Error: Cannot find '_' in file name\n");
    const size_t fname_it_delim = fname_begin->find(fdelim);

    vector<string> myfiles[maxUSB];
    map<int, int> mymap;
    int mapindex = 0;
    for(int k = 0; k<(int)initial_files.size(); k++) {
      string fusb = (initial_files[k]).substr(fname_it_delim+1, 2);
      const int iusb = (int)strtol(fusb.c_str(), NULL, 10);
      if(!mymap.count(iusb)) mymap[iusb] = mapindex++;
      myfiles[mymap[iusb]].push_back(initial_files[k]);
    }

    vector<string> files_to_rename;
    if(EBRunMode == kRecovery) {
      for(int j = 0; j < numUSB; j++) {
        const int mysize = myfiles[j].size();
        const int avgsize = initial_files.size()/numUSB;
        if(mysize > 0)
          files_to_rename.push_back(decoded_dir + myfiles[j].at(mysize - 1));
        if(mysize > 1)
          files_to_rename.push_back(decoded_dir + myfiles[j].at(mysize - 2));
        if(mysize > 2 && mysize > avgsize)
          files_to_rename.push_back(decoded_dir + myfiles[j].at(mysize - 3));
      }
    }
    else { // Rename all files if EBRunMode == kReprocess
      for(unsigned int j = 0; j < initial_files.size(); j++)
        files_to_rename.push_back(decoded_dir + initial_files[j]);
    }

    // Rename files
    for(int i = 0; i<(int)files_to_rename.size(); i++) {
      string fname = files_to_rename[i];
      {
        const size_t pos = fname.find("decoded");
        if(pos == string::npos)
          die_with_log("Unexpected decoded-data file name: %s\n",
            fname.c_str());
        fname.replace(pos, sizeof("decoded")-1, "binary");
      }
      {
        const size_t pos = fname.find(".done");
        if(pos == string::npos)
          die_with_log("Unexpected decoded-data file name: %s\n",
            fname.c_str());
        fname.replace(pos, sizeof(".done")-1, "");
      }

      while(rename(files_to_rename[i].c_str(), fname.c_str())) {
        log_msg(LOG_ERR, "Could not rename decoded data file.\n");
        sleep(1);
      }
    }
  }
}

static bool write_summary_table(long int lasttime, int subrun)
{
  mysqlpp::Connection myconn(false); // false to not throw exceptions on errors
  mysqlpp::StoreQueryResult res;

  if(!myconn.connect(database, server, username, password)) {
    log_msg(LOG_WARNING, "Cannot connect to MySQL database %s at %s\n",
      database, server);
    return false;
  }

  char query_string[BUFSIZE];

  sprintf(query_string, "UPDATE OV_runsummary SET EBcomment = '%ld', "
    "EBsubrunnumber = '%d' WHERE Run_number = '%s';",
    lasttime, subrun, RunNumber.c_str());
  mysqlpp::Query query = myconn.query(query_string);
  if(!query.execute()) {
    log_msg(LOG_WARNING, "MySQL query (%s) error: %s\n", query_string, query.error());
    myconn.disconnect();
    return false;
  }

  myconn.disconnect();
  return true;
}

static bool read_stop_time()
{
  mysqlpp::Connection myconn(false); // false to not throw exceptions on errors
  mysqlpp::StoreQueryResult res;

  if(!myconn.connect(database, server, username, password)) {
    log_msg(LOG_WARNING, "Cannot connect to MySQL database %s at %s\n",
      database, server);
    return false;
  }

  char query_string[BUFSIZE];

  sprintf(query_string, "SELECT stop_time FROM OV_runsummary "
    "WHERE Run_number = '%s' ORDER BY start_time DESC;", RunNumber.c_str());
  mysqlpp::Query query = myconn.query(query_string);
  res = query.store();
  if(res.num_rows() < 1) {
    log_msg(LOG_ERR, "MySQL query (%s) error: %s\n", query_string, query.error());
    myconn.disconnect();
    return false;
  }
  cout << "res[0][0]: " << res[0][0].c_str() << endl;
  if(res[0][0].is_null()) {
    myconn.disconnect();
    return false;
  }

  log_msg(LOG_INFO, "Found MySQL stop time for run: %s\n", RunNumber.c_str());
  myconn.disconnect();

  return true;
}

static bool write_endofrun_block(string myfname, int data_fd)
{
  cout << "Trying to write end of run block" << endl;
  if(EBRunMode == kRecovery || SubRunCounter % timestampsperoutput == 0) {
    data_fd = open_file(myfname);
    if(data_fd <= 0) {
      log_msg(LOG_ERR, "Cannot open file %s to write "
        "end-of-run block for run %s\n", myfname.c_str(), RunNumber.c_str());
      return false;
    }
  }

  OVEventHeader *CurrEventHeader = new OVEventHeader;
  CurrEventHeader->SetTimeSec(0);
  CurrEventHeader->SetNOVDataPackets(-99);

  if(write(data_fd, CurrEventHeader, sizeof(OVEventHeader)) < 0){
    log_msg(LOG_ERR, "End of run write error\n");
    return false;
  } // To be optimized

  if(close(data_fd) < 0) log_msg(LOG_ERR, "Could not close output data file\n");

  return true;
}

int main(int argc, char **argv)
{
  if(!parse_options(argc, argv)) {
    write_ebretval(-1);
    return 127;
  }

  DataVector ExtraDataVector; // DataVector carries over events from last time stamp
  vector<int> ExtraIndexVector;

  // Array of DataVectors for current timestamp to process
  DataVector CurrentDataVector[maxUSB];
  DataVector::iterator CurrentDataVectorIt[maxUSB];
  DataVector MinDataVector; // DataVector of current minimum data packets
  vector<int> MinDataPacket; // Minimum and Last Data Packets added
  vector<int> MinIndexVector; // Vector of USB index of Minimum Data Packet
  int dataFile = 0; // output file descriptor
  string fname; // output file name
  int EventCounter = 0;

  start_log(); // establish syslog connection

  // Load OV run_summary table
  // This should handle reprocessing eventually <-- relevant for CRT?
  read_summary_table();

  // Load baseline data
  time_t timeout = time(0);
  while(!GetBaselines()) { // Get baselines
    if((int)difftime(time(0), timeout) > MAXTIME) {
      log_msg(LOG_CRIT, "Error: Baseline data not found in last %d seconds.\n",
        MAXTIME);
      write_ebretval(-1);
      return 127;
    }
    else{
      sleep(2); // FixME: optimize? -- never done for Double Chooz -- needed?
    }
  }

  // Set Thresholds. Only for data streams
  for(int i = 0; i < num_nonFanUSB; i++)
    OVUSBStream[Datamap[i]].SetThresh(Threshold, (int)EBTrigMode);

  // Locate existing binary data and create output folder
  OutputFolder = OutputFolder + "Run_" + RunNumber;
  timeout = time(0);

  while(!LoadRun()) {
    if((int)difftime(time(0), timeout) > MAXTIME) {
      log_msg(LOG_CRIT, "Error: Binary data not found in the last %d seconds.\n",
        MAXTIME);
      write_ebretval(-1);
      return 127;
    }
    else sleep(1);
  }

  // This is the main Event Builder loop
  while(true) {

    for(int i=0; i < numUSB; i++) {

      DataVector *DataVectorPtr = &(CurrentDataVector[i]);

      while(!OVUSBStream[i].GetNextTimeStamp(DataVectorPtr)) {
        timeout = time(0);

        int status = 0;
        while((status = LoadAll()) < 1){ // Try to find new files for each USB
          if(status == -1) {
            write_ebretval(-1);
            return 127;
          }
          cout << "Files are not ready...\n";
          if((int)difftime(time(0), timeout) > ENDTIME) {
            if(read_stop_time() || (int)difftime(time(0), timeout) > MAXTIME) {
              while(!write_endofrun_block(fname, dataFile)) sleep(1);

              if((int)difftime(time(0), timeout) > MAXTIME)
                log_msg(LOG_ERR, "No data found for %d seconds!  "
                    "Closing run %s without finding stop time on MySQL\n",
                    MAXTIME, RunNumber.c_str());
              else
                log_msg(LOG_INFO, "Event Builder has finished processing run %s\n",
                  RunNumber.c_str());

              write_ebretval(1);
              return 0;
            }
          }
          sleep(1); // FixMe: optimize? -- never done for Double Chooz -- needed?
        }

        for(int j=0; j<numUSB; j++) { // Load all files in at once
          printf("Starting Thread %d\n", j);
          gThreads[j] = new TThread(Form("gThreads%d", j), handle, (void*) j);
          gThreads[j]->Run();
        }

        joinerThread = new TThread("joinerThread", joiner, (void*) numUSB);
        joinerThread->Run();

        while(!finished) sleep(1); // To be optimized -- never done for
                                   // Double Chooz -- needed?
        finished = false;

        joinerThread->Join();

        for(int k=0; k<numUSB; k++)
          if(gThreads[k])
            delete gThreads[k];
        if(joinerThread) delete joinerThread;

        cout << "Joined all threads!\n";

        // Rename files
        for(int i = 0; i<numUSB; i++) {
          string tempfilename = OVUSBStream[i].GetFileName();
          size_t mypos = tempfilename.find("binary");
          if(mypos != tempfilename.npos)
            tempfilename.replace(mypos, 6, "decoded");
          tempfilename += ".done";

          while(rename(OVUSBStream[i].GetFileName(), tempfilename.c_str())) {
            log_msg(LOG_ERR, "Could not rename binary data file.\n");
            sleep(1);
          }
        }
      }
    }

    // Open output data file
    // This should handle re-processing eventually
    if(EBRunMode == kRecovery) {
      if(OVUSBStream[0].GetTOLUTC() <= (unsigned long int)EBcomment) {
        printf("Time stamp to process: %ld\n", OVUSBStream[0].GetTOLUTC());
        printf("Recovery mode waiting to exceed time stamp %ld\n", EBcomment);
        dataFile = open_file("/dev/null");
      }
      else {
        printf("Run has been recovered!\n");
        EBRunMode = kNormal;
      }
    }
    if(EBRunMode != kRecovery && SubRunCounter % timestampsperoutput == 0) {
      fname = OutputFolder + "/DCRunF" + RunNumber;
      char subrun[BUFSIZE];
      sprintf(subrun, "%s%.5dOVDAQ", OVRunType.c_str(),
              SubRunCounter/timestampsperoutput);
      fname.append(subrun);
      dataFile = open_file(fname);
    }

    // index of minimum event added to USB stream
    int MinIndex=0;
    for(int i=0; i < numUSB; i++) {
      CurrentDataVectorIt[i]=CurrentDataVector[i].begin();
      if(CurrentDataVectorIt[i]==CurrentDataVector[i].end()) MinIndex = i;
    }
    MinDataVector.assign(ExtraDataVector.begin(), ExtraDataVector.end());
    MinIndexVector.assign(ExtraIndexVector.begin(), ExtraIndexVector.end());

    while( CurrentDataVectorIt[MinIndex]!=CurrentDataVector[MinIndex].end() ) {
      // Until 1 USB stream finishes timestamp

      MinIndex=0; // Reset minimum to first USB stream
      MinDataPacket = *(CurrentDataVectorIt[MinIndex]);

      for(int k=0; k<numUSB; k++) { // Loop over USB streams, find minimum

        vector<int> CurrentDataPacket = *(CurrentDataVectorIt[k]);

        // Find real minimum; no clock slew
        if( LessThan(CurrentDataPacket, MinDataPacket, 0) ) {
          // If current packet less than min packet, min = current
          MinDataPacket = CurrentDataPacket;
          MinIndex = k;
        }

      } // End of for loop: MinDataPacket has been filled appropriately

      if(MinDataVector.size() > 0) { // Check for equal events
        if( LessThan(MinDataVector.back(), MinDataPacket, 3) ) {
          // Ignore gaps which have consist of fewer than 4 clock cycles

          ++EventCounter;
          BuildEvent(&MinDataVector, &MinIndexVector, dataFile);

          MinDataVector.clear();
          MinIndexVector.clear();
        }
      }
      MinDataVector.push_back(MinDataPacket); // Add new element
      MinIndexVector.push_back(MinIndex);
      CurrentDataVectorIt[MinIndex]++; // Increment iterator for added packet

    } // End of while loop: Events have been built for this time stamp

    // Clean up operations and store data for later
    for(int k=0; k<numUSB; k++)
      CurrentDataVector[k].assign(CurrentDataVectorIt[k], CurrentDataVector[k].end());
    ExtraDataVector.assign(MinDataVector.begin(), MinDataVector.end());
    ExtraIndexVector.assign(MinIndexVector.begin(), MinIndexVector.end());

    if(EBRunMode == kRecovery) {
      if(close(dataFile) < 0) {
        log_msg(LOG_ERR,
          "Fatal Error: Could not close output data file in recovery mode!\n");
        write_ebretval(-1);
        return 127;
      }
    }
    else {
      ++SubRunCounter;
      if((SubRunCounter % timestampsperoutput == 0) && dataFile) {
        if(close(dataFile) < 0) {
          log_msg(LOG_CRIT, "Fatal Error: Could not close output data file!\n");
          write_ebretval(-1);
          return 127;
        }
        else {
          while(!write_summary_table(OVUSBStream[0].GetTOLUTC(),
                                     SubRunCounter/timestampsperoutput)) {
            log_msg(LOG_NOTICE, "Error writing to OV_runsummary table.\n");
            sleep(1);
          }
        }
      }
    }

    cout << "Number of Merged Muon Events: " << EventCounter << endl;
    cout << "Processed Time Stamp: " << OVUSBStream[0].GetTOLUTC() << endl;
    EventCounter = 0;
  }

  cout << "Normally this program should not terminate like it is now...\n";

  write_ebretval(1);
  return 0;
}
