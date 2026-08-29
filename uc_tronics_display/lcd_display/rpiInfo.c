#include "rpiInfo.h"
#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/vfs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <fcntl.h>
#include "st7735.h"
#include <stdlib.h>

/*
* Get the IP address of wlan0 or eth0
*/

char* get_ip_address(void)
{
    /* FIX: this asked the kernel for the address of an interface literally
       named "eth0", and returned "xxx.xxx.xxx.xxx" when that failed. The name
       is not portable: recent Raspberry Pi kernels call the built-in NIC end0,
       Ubuntu still uses eth0, and systemd elsewhere produces enp*/ens*. Home
       Assistant OS on a Pi therefore showed no address at all.

       (Before that it returned a hardcoded string, which worked only because
       the addon container could not have seen the host's address anyway.)

       Now every interface is enumerated and the first usable IPv4 wins:
       skipping loopback and down interfaces, preferring wired over wireless
       unless IPADDRESS_TYPE says otherwise. UCTRONICS_IP_ADDRESS still
       overrides everything. */
    static char address[INET_ADDRSTRLEN];
    struct ifaddrs *ifaddr, *ifa;
    int pass;

    const char *override = getenv("UCTRONICS_IP_ADDRESS");
    if (override && *override)
    {
      return (char *)override;
    }

    if (getifaddrs(&ifaddr) == -1)
    {
      return "xxx.xxx.xxx.xxx";
    }

    /* Pass 0 takes the preferred family of interface, pass 1 takes anything.
       Wireless names begin with 'w' (wlan0, wlp2s0) on every scheme in use. */
    for (pass = 0; pass < 2; pass++)
    {
      for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
      {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
        {
          continue;
        }
        if ((ifa->ifa_flags & IFF_LOOPBACK) || !(ifa->ifa_flags & IFF_UP))
        {
          continue;
        }
        if (pass == 0)
        {
          int wireless = (ifa->ifa_name[0] == 'w');
          if (IPADDRESS_TYPE == WLAN0_ADDRESS ? !wireless : wireless)
          {
            continue;
          }
        }

        if (inet_ntop(AF_INET,
                      &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                      address, sizeof(address)) != NULL)
        {
          freeifaddrs(ifaddr);
          return address;
        }
      }
    }

    freeifaddrs(ifaddr);
    return "xxx.xxx.xxx.xxx";
}


/*
* get ram memory
*
* FIX: previously read MemFree, which excludes reclaimable page cache and so
* makes a healthy Linux box look ~98% full. MemAvailable is the kernel's own
* estimate of memory obtainable without swapping, which is what a "free RAM"
* display should show. Falls back to MemFree on ancient kernels that lack it.
*/
void get_cpu_memory(float *Totalram,float *freeram)
{
  struct sysinfo s_info;

  unsigned int value=0;
  unsigned char buffer[100]={0};
  unsigned char famer[100]={0};
  int got_available = 0;
  float memfree_fallback = 0.0f;

    if(sysinfo(&s_info)==0)            //Get memory information
    {
        FILE* fp=fopen("/proc/meminfo","r");
        if(fp==NULL)
        {
            return ;
        }
        while(fgets((char *)buffer,sizeof(buffer),fp))
        {
            if(sscanf((char *)buffer,"%s%u",famer,&value)!=2)
            {
            continue;
            }
            if(strcmp((char *)famer,"MemTotal:")==0)
            {
             *Totalram=value/1000.0/1000.0;
            }
            else if(strcmp((char *)famer,"MemAvailable:")==0)
            {
              *freeram=value/1000.0/1000.0;
              got_available = 1;
            }
            else if(strcmp((char *)famer,"MemFree:")==0)
            {
              memfree_fallback=value/1000.0/1000.0;
            }
        }
        fclose(fp);

        if(!got_available)
        {
          *freeram = memfree_fallback;
        }
    }
}

/*
* get sd memory
*
* NOTE: statfs("/") inside the add-on container reports the container's own
* root overlay, not the host SD/SSD. Left as-is because nothing in the display
* path appears to rely on it; if it is ever used, point it at "/data".
*/
void get_sd_memory(uint32_t *MemSize, uint32_t *freesize)
{
    struct statfs diskInfo;
    statfs("/",&diskInfo);
    unsigned long long blocksize = diskInfo.f_bsize;// The number of bytes per block
    unsigned long long totalsize = blocksize*diskInfo.f_blocks;//Total number of bytes
    *MemSize=(unsigned int)(totalsize>>30);


    unsigned long long size = blocksize*diskInfo.f_bfree; //Now let's figure out how much space we have left
    *freesize=size>>30;
    *freesize=*MemSize-*freesize;
}


/*
* get hard disk memory
*
* FIX: the original shelled out to `df | grep /dev/sda`, which inside this
* container matches FIVE bind-mounted lines all on the same device. awk then
* printed field 2 for every line with no separator, producing a concatenated
* number that the 10-byte buffer truncated mid-digits — hence "134% used".
* It also called fclose() on a popen() stream, which never reaps the child,
* leaking a zombie process on every refresh.
*
* statfs() answers the same question directly: no subprocess, no busybox
* parsing differences, no zombies. "/data" is the add-on's mount of the host
* data partition (sda8 on this machine), which is the volume worth showing.
*/
uint8_t get_hard_disk_memory(uint16_t *diskMemSize, uint16_t *useMemSize)
{
  struct statfs diskInfo;

  *diskMemSize = 0;
  *useMemSize  = 0;

  if (statfs("/data", &diskInfo) != 0)
  {
    return 0;
  }

  unsigned long long blocksize = diskInfo.f_bsize;
  unsigned long long totalsize = blocksize * (unsigned long long)diskInfo.f_blocks;
  unsigned long long freesize  = blocksize * (unsigned long long)diskInfo.f_bfree;

  *diskMemSize = (uint16_t)(totalsize >> 30);                 /* GiB total */
  *useMemSize  = (uint16_t)((totalsize - freesize) >> 30);    /* GiB used  */

  return 0;
}

/*
* get temperature
*/

uint8_t get_temperature(void)
{
    FILE *fd;
    unsigned int temp;
    char buff[10] = {0};
    fd = fopen("/sys/class/thermal/thermal_zone0/temp","r");
    if (fd == NULL)
    {
      return 0;
    }
    fgets(buff,sizeof(buff),fd);
    sscanf(buff, "%d", &temp);
    fclose(fd);
    return TEMPERATURE_TYPE == FAHRENHEIT ? temp/1000*1.8+32 : temp/1000;
}

/*
* Get cpu usage
*
* FIX: busybox top has no "%Cpu" line — it prints
*   CPU:   2% usr   2% sys   0% nic  95% idle   0% io   0% irq   0% sirq
* so the original `grep %Cpu` matched only its own command line in the process
* list, and awk read the PPID column as a percentage. This finds the "idle"
* field by name and returns 100-idle, so it survives field reordering.
* Also uses pclose() (the original used fclose(), leaking the child).
*/
uint8_t get_cpu_message(void)
{
    /* FIX: this used to run `top -bn1 | awk '/^CPU:/...'` through popen and
       parse an "idle" field. That is BUSYBOX top's format -- correct in the
       Alpine addon container, and silently wrong everywhere else: procps top
       on Debian/Ubuntu prints "%Cpu(s): ... 98.3 id," with no "idle" token, so
       the awk matched nothing, atoi("") returned 0, and the bar sat at its
       minimum no matter how loaded the machine was.

       /proc/stat is the source top itself reads. No fork, no shell, no output
       format to parse -- and it removes a popen from the slowest screen, which
       is also the only one that repaints the whole display.

       Deltas are kept between calls, so this reports load since the previous
       refresh rather than since boot. The first call has no previous sample
       and reports 0. */
    static unsigned long long prev_total = 0, prev_idle = 0;

    FILE *fp;
    char buff[256];
    unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
    unsigned long long total, idle_all, d_total, d_idle;

    fp = fopen("/proc/stat", "r");
    if (fp == NULL)
    {
      return 0;
    }
    if (fgets(buff, sizeof(buff), fp) == NULL)
    {
      fclose(fp);
      return 0;
    }
    fclose(fp);

    if (sscanf(buff, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) < 4)
    {
      return 0;
    }

    idle_all = idle + iowait;
    total = user + nice + sys + idle_all + irq + softirq + steal;

    if (prev_total == 0 || total <= prev_total)
    {
      prev_total = total;
      prev_idle = idle_all;
      return 0;
    }

    d_total = total - prev_total;
    d_idle  = idle_all - prev_idle;
    prev_total = total;
    prev_idle = idle_all;

    return (uint8_t)((100ULL * (d_total - d_idle)) / d_total);
}

