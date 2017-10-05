#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <string.h>
#include <netdb.h>
#include <syslog.h>
#include <dirent.h>
#include <errno.h>
#include "USBstreamUtils.h"

static char val[BUFSIZE];
static char gaibu_debug_msg[BUFSIZE];

char* config_string(const char* path, const char* key)
{
  char ch[300];
  char name[200];

  char IP[200];
  char port[200];

  FILE * file;
  file = fopen(path,"r");
  if (file != NULL) {
    while( fgets( ch, 120, file ) != NULL ) {
      if (sscanf(ch, "%s :%s", name, IP) == 2) { // get IP/hostname
        if (  strcmp(name, key) == 0 ) {
          sprintf(val,IP);
          fclose(file);
          return val;
        }
      }
      else if (sscanf(ch, "%s %s", name, port) == 2){ // get port number
        if ( strcmp(name, key) == 0 ) {
          sprintf(val,port);
          fclose(file);
          return val;
        }
      }

    }
    fclose(file);
  }
  else {
    printf("Unable to open file %s\n",path);
    exit(1);
  }
  return NULL;
}

// Print message and write to the syslog if sufficiently important
void gaibu_msg(int priority, char *gaibu_buf, std::string myRunNumber)
{
  printf("%s\n", gaibu_buf);

  if(priority == LOG_NOTICE || priority == LOG_WARNING || priority == LOG_ERR ||
     priority == LOG_CRIT   || priority == LOG_ALERT   || priority == LOG_EMERG)
    syslog(priority, gaibu_buf);
}

bool LessThan(DataPacket lhs, DataPacket rhs, int ClockSlew)
{
  if( lhs.size() < 7 || rhs.size() < 7) {
    sprintf(gaibu_debug_msg,"Vector size error! Could not compare OV Hits");
    gaibu_msg(LOG_ERR,gaibu_debug_msg);
    exit(1);
  }

  const long int dt_high = (lhs[1]<<8) + lhs[2] - (rhs[1]<<8) - rhs[2];
  const long int dt_low  = (lhs[3]<<8) + lhs[4] - (rhs[3]<<8) - rhs[4];
  const long int dt_16ns_high = lhs[5] - rhs[5];
  const long int dt_16ns_low  = lhs[6] - rhs[6];

  if(dt_high != 0) return dt_high < 0; // Very different timestamps

  if(labs(dt_low) > 1) return dt_low < 0; // Timestamps are not adjacent

  // Was sync pulse 2sec (sqrd)
  if(labs(dt_16ns_high) > 2000) return dt_16ns_high > 0;

  if(dt_16ns_high != 0) return dt_16ns_high < 0; // Order hi 16 bits of clock counter

  return dt_16ns_low < -ClockSlew; // Order lo 16 bits of clock counter
}

void start_gaibu()
{
  openlog("OV EBuilder", LOG_NDELAY, LOG_USER);

  gaibu_msg(LOG_NOTICE, "OV Event Builder Started");
}

int GetDir(std::string dir, std::vector<std::string> &myfiles,
                  int opt, int opt2)
{
  DIR *dp;
  struct dirent *dirp;
  std::string myfname;

  errno = 0;
  if((dp = opendir(dir.c_str())) == NULL) {
    closedir(dp);
    return errno;
  }

  int counter = 0;
  do{
    if((dirp = readdir(dp)) != NULL) {
      counter++;
      myfname = std::string(dirp->d_name);

      // extensions reserved
      if((myfname.find(".") == myfname.npos || (opt && myfname.size() > 2)) &&
         (opt2 || (myfname.find("baseline") == myfname.npos &&
         myfname.find("processed") == myfname.npos) ) )
        myfiles.push_back(myfname);
    }
  } while(dirp!=NULL);

  if(closedir(dp) < 0)
    return errno;

  dp=NULL;

  if(myfiles.size()==0)
    return -1;
  else
    return 0;
}
