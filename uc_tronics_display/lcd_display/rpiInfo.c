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
    /* Which address is "the host's" is a routing question, so ask the routing
       table rather than guessing from interface names.

       Two earlier attempts got this wrong. The first asked for an interface
       literally named "eth0" -- true on Ubuntu, but Raspberry Pi kernels call
       it end0 and systemd elsewhere uses enp-style names, so Home Assistant OS
       showed nothing. The second took the first non-loopback IPv4 from
       getifaddrs(), which fails two ways: docker0 is a perfectly good
       non-loopback IPv4 that is nobody's idea of the host's address, and
       getifaddrs() itself needs an AF_NETLINK socket -- so under a systemd
       unit with RestrictAddressFamilies it returns -1 and reports nothing at
       all. That is what happened outside the addon container, where no such
       sandbox applies.

       Connecting a UDP socket sends no packets. It just makes the kernel pick
       the source address it would use to reach the internet, which is exactly
       the definition wanted, and it is immune to interface naming, bridge
       interfaces and VPN interfaces alike. It also needs only AF_INET, which
       a hardened unit is likely to permit already since the display is on an
       IP network by definition.

       UCTRONICS_IP_ADDRESS still overrides, for hosts where that answer is not
       the useful one. */
    static char address[INET_ADDRSTRLEN];
    struct sockaddr_in probe, local;
    socklen_t len = sizeof(local);
    int fd;

    const char *override = getenv("UCTRONICS_IP_ADDRESS");
    if (override && *override)
    {
      return (char *)override;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
      return "xxx.xxx.xxx.xxx";
    }

    memset(&probe, 0, sizeof(probe));
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    /* Never contacted -- a UDP connect() only sets the socket's peer and lets
       the kernel resolve a route. Any routable address works. */
    probe.sin_addr.s_addr = inet_addr("1.1.1.1");

    if (connect(fd, (struct sockaddr *)&probe, sizeof(probe)) < 0 ||
        getsockname(fd, (struct sockaddr *)&local, &len) < 0)
    {
      close(fd);
      return "xxx.xxx.xxx.xxx";
    }
    close(fd);

    if (inet_ntop(AF_INET, &local.sin_addr, address, sizeof(address)) == NULL)
    {
      return "xxx.xxx.xxx.xxx";
    }
    return address;
}


/*
* Get this host's name, for the placeholder shown while the display waits to
* join the aligned rotation.
*
* UCTRONICS_HOSTNAME overrides, and inside the addon it has to: host_network
* shares the host's NETWORK namespace but not its UTS namespace, so
* gethostname() there still answers with the container's generated name rather
* than the machine's. Same shape as the IP override above, and for the same
* reason -- the container cannot always see what the display is meant to show.
*/
char* get_host_name(void)
{
    static char name[64];

    const char *override = getenv("UCTRONICS_HOSTNAME");
    if (override && *override)
    {
      return (char *)override;
    }

    if (gethostname(name, sizeof(name)) != 0)
    {
      return "unknown";
    }
    /* POSIX does not promise a terminator when the name is truncated. */
    name[sizeof(name) - 1] = '\0';
    return name;
}


/*
* Is the system clock synchronised to a time source?
*
* Returns 1 for yes, 0 for no, and -1 for "cannot tell".
*
* The rack Pis have no RTC: at boot the clock reads roughly whenever the
* filesystem was last written, and systemd-timesyncd corrects it seconds
* later. Anything that pins itself to the wall clock -- which is exactly what
* the aligned display loop does -- has to wait for that, or it pins itself to a
* fiction and jumps when the correction lands.
*
* timedatectl is what a human would run and what systemd considers the answer.
* popen() is acceptable here where it is not in a render path: this runs once
* at startup, then at most twice a minute while the clock is still wrong, never
* per refresh. Where there is no systemd at all -- the addon container -- the
* command is simply missing, the shell exits 127, and the caller is told
* "cannot tell" rather than made to wait for an answer that is never coming.
*/
int get_ntp_synchronised(void)
{
    FILE *fp;
    char buff[32] = {0};
    int got;

    fp = popen("timedatectl show -p NTPSynchronized --value 2>/dev/null", "r");
    if (fp == NULL)
    {
      return -1;
    }
    got = (fgets(buff, sizeof(buff), fp) != NULL);
    /* pclose(), not fclose() -- fclose() on a popen() stream never reaps the
       child, which is the zombie leak fixed elsewhere in this file. */
    if (pclose(fp) != 0 || !got)
    {
      return -1;
    }

    return strncmp(buff, "yes", 3) == 0;
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

