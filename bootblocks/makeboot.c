
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#include "sysboot.v"
#include "noboot.v"
#include "msdos.v"
#include "skip.v"
#include "tarboot.v"
#include "minix.v"
#include "minixhd.v"
#include "mbr.v"

unsigned char buffer[1024];

#define FS_NONE	0	/* Bootsector is complete */
#define FS_ADOS	1	/* Bootsector needs 'normal' DOS FS */
#define FS_DOS	2	/* Bootsector needs any DOS FS */
#define FS_TAR	3	/* Bootsector needs GNU-tar volume label */
#define FS_STAT	4	/* DOS bootsector is checked */
#define FS_ZERO 5	/* Boot sector must be Zapped */
#define FS_MBR  6	/* Boot sector is an MBR */

struct bblist {
   char * name;
   char * desc;
   char * data;
   int	  size;
   int	fstype;
} bblocks[] = {
{ "tar",  "Bootable GNU tar volume lable", tarboot_data, tarboot_size, FS_TAR},
{ "dosfs","Boot file BOOTFILE.SYS from dosfs", msdos_data, msdos_size, FS_ADOS},
{ "none", "No OS bootblock, just message",   noboot_data, noboot_size, FS_DOS},
{ "skip", "Bypasses floppy boot with message",   skip_data, skip_size, FS_DOS},
{ "minix","Minix floppy FS booter",            minix_data, minix_size, FS_ZERO},
{ "hdmin","Minix Hard disk FS booter",     minixhd_data, minixhd_size, FS_ZERO},
{ "mbr",  "Master boot record for HD",              mbr_data,mbr_size, FS_MBR},
{ "stat", "Display dosfs superblock",                           0,  0, FS_STAT},
{ "copy", "Copy boot block to makeboot.sav",                    0,  0, FS_STAT},
{ "Zap",  "Clear boot block to NULs",                        0,  1024, FS_NONE},
   0
};

char * progname = "";

int disktype = 0;
FILE * diskfd;

int disk_sect = 63;	/* These are initilised to the maximums */
int disk_head = 256;	/* Set to the correct values when an MSDOS disk is */
int disk_trck = 256;	/* successfully identified */

int force = 0;
int write_zero = 1;	/* Write sector 0 */
int write_one = 0;	/* Write sector 1 */
int bs_offset = 0;	/* Offset of _real_ bootsector for 2m floppies */

main(argc, argv)
int argc;
char ** argv;
{
   FILE * fd;
   struct bblist *ptr;
   int i;

   progname = argv[0];

   if( argc == 4 && strcmp(argv[1], "-f") == 0 )
   {
      argv++; argc--; force++;
   }
   if( argc != 3 ) Usage();

   if( (i=strlen(argv[1])) < 2 ) Usage();
   for(ptr = bblocks; ptr->name; ptr++)
      if( strncmp(argv[1], ptr->name, i) == 0 ) break;
   if( ptr->name == 0 ) Usage();

   open_disk(argv[2]);
   if( read_sector(0, buffer) != 0 )
      exit(1);
   read_sector(1, buffer+512);

   write_zero = (ptr->size >= 512);
   write_one =  (ptr->size >= 1024);

   switch(ptr->fstype)
   {
   case FS_NONE:	/* override */
   	break;
   case FS_ADOS:
      check_simpledos();
      break;
   case FS_DOS:
   case FS_STAT:
      check_msdos();
      break;
   case FS_TAR:
      check_tar();
      break;
   case FS_ZERO:
      check_zapped();
      break;
   case FS_MBR:
      check_mbr();
      break;

   default:
      fprintf(stderr, "Program error, unknown filesystem requirement\n");
      exit(2);
   }

   switch(ptr->fstype)
   {
   case FS_STAT:
      print_super(buffer);
      if( strcmp(ptr->name, "copy") == 0 )
         save_super(buffer);
      close_disk();
      exit(0);
   case FS_ADOS:
   case FS_DOS:
      for(i=0; i<sysboot_dosfs_stat; i++)
         buffer[i] = ptr->data[i];
      for(i=sysboot_codestart; i<512; i++)
         buffer[i] = ptr->data[i];
      break;

   case FS_TAR:
      copy_tarblock();
      break;

   case FS_MBR:
      copy_mbr(ptr->data);
      break;

   case FS_NONE:
      if( ptr->data )
	 memcpy(buffer, ptr->data, 512);
      else
      {
	 memset(buffer, '\0', 1024);
	 write_one = 1;
      }
      break;
   }

   if( bs_offset )
   {
      if( write_zero ) do_2m_write();
      /* Don't write 1 ever! */
   }
   else
   {
      if( write_zero ) write_sector(0, buffer);
      if( write_one )  write_sector(1, buffer+512);
   }
   close_disk();
   exit(0);
}

Usage()
{
   struct bblist *ptr = bblocks;

   if( progname == 0 || *progname == 0 || progname[1] == 0 )
      progname = "makeboot";

#ifdef __MSDOS__
   fprintf(stderr, "Usage: %s [-f] bootname a:\n", progname);
#else
   fprintf(stderr, "Usage: %s [-f] bootname /dev/fd0\n", progname);
#endif
   fprintf(stderr, "Blocks\n");
   for(;ptr->name; ptr++)
       fprintf(stderr, "\t%s\t%s\n", ptr->name, ptr->desc);
   exit(1);
}

/**************************************************************************/

int
open_disk(diskname)
char * diskname;
{
#ifdef __MSDOS__
   if( strcmp("a:", diskname) == 0 ) { disktype = 1; return 0; }
   if( strcmp("b:", diskname) == 0 ) { disktype = 2; return 0; }
   if( strcmp("A:", diskname) == 0 ) { disktype = 1; return 0; }
   if( strcmp("B:", diskname) == 0 ) { disktype = 2; return 0; }
#endif
   disktype = 0;
   diskfd = fopen(diskname, "r+");
   if( diskfd == 0 )
   {
      fprintf(stderr, "Cannot open %s\n", diskname);
      exit(1);
   }
   return 0;
}

close_disk()
{
   if( diskfd && disktype == 0 ) fclose(diskfd);
   diskfd = 0;
   disktype = 0;
}

int
write_sector(sectno, loadaddr)
int sectno;
char * loadaddr;
{
#ifdef __MSDOS__
   if( disktype == 1 || disktype == 2 )
   {
      int tries, rv;
      int s,h,c;
      s = sectno%disk_sect + 1;
      h = sectno/disk_sect%disk_head;
      c = sectno/disk_sect/disk_head;

      for(tries=0; tries<6; tries++)
         if( (rv = dos_sect_write(disktype-1, c, h, s, loadaddr)) == 0 )
            break;
      if( rv )
      {
         fprintf(stderr, "Error writing sector %d, (%d)\n", sectno, rv/256);
	 return -1;
      }
      return 0;
   }
#endif
   if( disktype )
   {
      fprintf(stderr, "Cannot write sector %d\n", sectno);
      return -1;
   }
   fseek(diskfd, (long)sectno*512, 0);
   if( fwrite(loadaddr, 512, 1, diskfd) != 1 )
   {
      fprintf(stderr, "Cannot write sector %d\n", sectno);
      return -1;
   }
   return 0;
}

int
read_sector(sectno, loadaddr)
int sectno;
char * loadaddr;
{
   int cc;
#ifdef __MSDOS__
   if( disktype == 1 || disktype == 2 )
   {
      int tries, rv;
      int s,h,c;
      s = sectno%disk_sect + 1;
      h = sectno/disk_sect%disk_head;
      c = sectno/disk_sect/disk_head;

      for(tries=0; tries<6; tries++)
         if( (rv = dos_sect_read(disktype-1, c, h, s, loadaddr)) == 0 )
            break;
      if( rv )
      {
         fprintf(stderr, "Error reading sector %d, (%d)\n", sectno, rv/256);
         memset(loadaddr, '\0', 512);
	 return -1;
      }
      return 0;
   }
#endif
   if( disktype )
   {
      fprintf(stderr, "Cannot read sector %d\n", sectno);
      return -1;
   }
   fseek(diskfd, (long)sectno*512, 0);
   if( (cc=fread(loadaddr, 1, 512, diskfd)) != 512 )
   {
      fprintf(stderr, "Cannot read sector %d, clearing\n", sectno);
      if(cc<0) cc=0;
      memset(loadaddr+cc, '\0', 512-cc);
   }
   return 0;
}
/**************************************************************************/

#ifdef __MSDOS__
dos_sect_read(drv, track, head, sector, loadaddr)
{
#asm
  push	bp
  mov	bp,sp

  push	ds
  pop	es

  mov	dh,[bp+2+_dos_sect_read.head]
  mov	dl,[bp+2+_dos_sect_read.drv]
  mov	cl,[bp+2+_dos_sect_read.sector]
  mov	ch,[bp+2+_dos_sect_read.track]

  mov	bx,[bp+2+_dos_sect_read.loadaddr]

  mov	ax,#$0201
  int	$13
  jc	read_err
  mov	ax,#0
read_err:

  pop	bp
#endasm
}
#endif

#ifdef __MSDOS__
dos_sect_write(drv, track, head, sector, loadaddr)
{
#asm
  push	bp
  mov	bp,sp

  push	ds
  pop	es

  mov	dh,[bp+2+_dos_sect_write.head]
  mov	dl,[bp+2+_dos_sect_write.drv]
  mov	cl,[bp+2+_dos_sect_write.sector]
  mov	ch,[bp+2+_dos_sect_write.track]

  mov	bx,[bp+2+_dos_sect_write.loadaddr]

  mov	ax,#$0301
  int	$13
  jc	write_err
  mov	ax,#0
write_err:

  pop	bp
#endasm
}
#endif

/**************************************************************************/

check_zapped()
{
   int i;
   for(i=0; i<512; i++)
      if( buffer[i] )
         break;

   if( i != 512 )
   {
      fprintf(stderr, "Boot block isn't empty, zap it first\n");
      if(!force) exit(1);
   }
}

/**************************************************************************/

struct tar_head {
   char name[100];
   char mode[8];
   char uid[8];
   char gid[8];
   char size[12];
   char mtime[12];
   char chksum[8];
   char linkflag;
   char linkname[100];
   char magic[8];
   char uname[32];
   char gname[32];
   char devmajor[8];
   char devminor[8];
   char padding[167];
} ;

#define buff_tar	(*(struct tar_head*) buffer)
#define boot_tar	(*(struct tar_head*) tarboot_data)

unsigned int oct(s)
char *s;
{
   unsigned int val = 0;
   int i;
   for(i=0; i<8; i++) if( s[i] >= '0' && s[i] <= '7' )
      val = (val<<3) + s[i] - '0';
   return val;
}

check_tar()
{
   char vbuf[100];
   unsigned char *p;
   unsigned int csum = 0;
   long osum = -1;

   for(p=buffer; p<buffer+512; p++)
      if( *p ) goto not_zapped;
   /* Block zapped, ok */
   return 0;
not_zapped:

   osum = oct(buff_tar.chksum);
   memset(buff_tar.chksum, ' ', sizeof(buff_tar.chksum));

   for(p=buffer; p<buffer+512; p++)
      csum += (*p & 0xFF);

   if( csum != osum )
   {
      fprintf(stderr, "TAR file checksum failed, this isn't a tar file.\n");
      if(!force) exit(9);

      write_one = 1;
      memset(buffer, '\0', 1024);
   }
   if( buff_tar.linkflag != 'V' )
   {
      fprintf(stderr, "Tar file doesn't start with a volume label\n");
      if(!force) exit(8);
   }

   strcpy(vbuf, boot_tar.name); strcat(vbuf, " Volume 1");
   if( strcmp(boot_tar.name, buff_tar.name) != 0 
    && strcmp(vbuf, buff_tar.name) != 0 )
   {
      fprintf(stderr, "WARNING: Volume is labeled as '%s' not '%s'\n",
                       buff_tar.name, boot_tar.name);
   }
   return 0;
}

copy_tarblock()
{
   char lbuf[20];
   unsigned char * p;
   unsigned int csum = 0;
   int i;

   struct tar_head temp;

   temp = boot_tar;

   /* Copy preserved fields
    */
   if( buff_tar.name[0] )
   {
      memcpy(temp.mtime, buff_tar.mtime, sizeof(temp.mtime));

      memset(temp.name, 0x90, 16);
      for(i=0; buff_tar.name[i] && buff_tar.name[i] != ' ' && i<14; i++)
      {
         int ch = buff_tar.name[i];
         if( islower(ch) ) ch = toupper(ch);
         if( strchr("/?@ABCDEFGHIJKLMNO", ch) == 0 )
            ch = '?';
         temp.name[i] = ch;
      }
      temp.name[i++] = 0;
      temp.name[i]   = 0xC0;
   }
   else
      sprintf(temp.mtime, "%11lo", time((void*)0));

   buff_tar = temp;

   /* Re-calculate the checksum */
   memset(buff_tar.chksum, ' ', sizeof(buff_tar.chksum));

   for(p=buffer; p<buffer+512; p++)
      csum += (*p & 0xFF);

   sprintf(buff_tar.chksum, "%7o", csum);

   printf("Boot block installed");
   if( ((struct tar_head*)buffer)[1].name[0] )
      printf(" to boot file '%s'\n",
         ((struct tar_head*)buffer)[1].name);
   else
      printf(", use 'tar -r' to add executable\n");
}

/**************************************************************************/

#define DOS_SYSID	0
#define DOS_SECT	1
#define DOS_CLUST	2
#define DOS_RESV	3
#define DOS_NFAT	4
#define DOS_NROOT	5
#define DOS_MAXSECT	6
#define DOS_MEDIA	7
#define DOS_FATLEN	8
#define DOS_SPT		9
#define DOS_HEADS	10
#define DOS_HIDDEN	11
#define DOS4_MAXSECT	12
#define DOS4_PHY_DRIVE	13
#define DOS4_SERIAL	14
#define DOS4_LABEL	15
#define DOS4_FATTYPE	16

struct bootfields {
   int offset;
   int length;
   int value;
}
   dosflds[] =
{
   { 0x03, 8, 0},
   { 0x0B, 2, 0},
   { 0x0D, 1, 0},
   { 0x0E, 2, 0},
   { 0x10, 1, 0},
   { 0x11, 2, 0},
   { 0x13, 2, 0},
   { 0x15, 1, 0},
   { 0x16, 2, 0},
   { 0x18, 2, 0},
   { 0x1A, 2, 0},
   { 0x1C, 4, 0},
   { 0x20, 4, 0},
   { 0x24, 1, 0},
   { 0x27, 4, 0},
   { 0x2B, 11, 0},
   { 0x36, 8, 0},
   { -1,0,0}
};

print_super(bootsect)
char * bootsect;
{
static char * fieldnames[] = {
   "System ID",
   "Sector size",
   "Cluster size",
   "Reserved sectors",
   "FAT count",
   "Root dir entries",
   "Sector count (=0 if large FS)",
   "Media code",
   "FAT length",
   "Sect/Track",
   "Heads",
   "Hidden sectors (Partition offset)",
   "Large FS sector count",
   "Phys drive",
   "Serial number",
   "Disk Label (DOS 4+)",
   "FAT type",
   0
};
   int i;

   for(i=0; dosflds[i].offset >= 0; i++)
   {
      printf("%-35s", fieldnames[i]);
      if( dosflds[i].length <= 4 )
      {
         long v = 0; int j;
	 for(j=dosflds[i].length-1; j>=0; j--)
	 {
	    v = v*256 + (0xFF&( bootsect[dosflds[i].offset+j] ));
	 }
	 printf("%ld\n", v);
      }
      else
      {
         int ch, j;
	 for(j=0; j<dosflds[i].length; j++)
	 {
	    ch = bootsect[dosflds[i].offset+j];
	    if( ch <= ' ' || ch > '~' ) putchar('.');
	    else                        putchar(ch);
	 }
	 putchar('\n');
      }
   }
}

decode_super(bootsect)
char * bootsect;
{
   int i;

   for(i=0; dosflds[i].offset >= 0; i++)
   {
      if( dosflds[i].length <= 4 )
      {
         long v = 0; int j;
	 for(j=dosflds[i].length-1; j>=0; j--)
	 {
	    v = v*256 + (0xFF&( bootsect[dosflds[i].offset+j] ));
	 }
	 dosflds[i].value = v;
      }
      else
	 dosflds[i].value = 0;
   }
}

save_super(bootsect)
char * bootsect;
{
   FILE * fd;
   fd = fopen("makeboot.sav", "wb");
   fwrite(bootsect, 1024, 1, fd);
   fclose(fd);
}

/**************************************************************************/

check_msdos()
{
   decode_super(buffer);
   if( dosflds[DOS_CLUST].value == 0 )	/* MSDOS v1.0 */
      dosflds[DOS_CLUST].value = 1;

   if( dosflds[DOS_MEDIA].value < 0xF0 )
      fprintf(stderr, "Dos media descriptor is invalid\n");
   else if( dosflds[DOS_MEDIA].value != (0xFF&buffer[512])
         && dosflds[DOS_RESV].value == 1 )
      fprintf(stderr, "Dos media descriptor check failed\n");
   else
   {
      disk_sect = dosflds[DOS_SPT].value;
      disk_head = dosflds[DOS_HEADS].value;
      if( disk_sect > 0 && disk_head > 0 )
         disk_trck = dosflds[DOS_MAXSECT].value/disk_head/disk_sect;

#ifndef __MSDOS__
      if( bs_offset == 0 &&
          memcmp(buffer+dosflds[DOS_SYSID].offset, "2M-STV0", 7) == 0)
      {
         printf("Floppy is in 2M format - reading 2nd boot block\n");
         bs_offset = dosflds[DOS_RESV].value + dosflds[DOS_FATLEN].value;
         if( read_sector(bs_offset, buffer) != 0 )
            exit(1);

         decode_super(buffer);
         if( dosflds[DOS_MEDIA].value < 0xF0 ||
                ( dosflds[DOS_MEDIA].value != (0xFF&buffer[512])
               && dosflds[DOS_RESV].value == 1 ) )
	 {
            printf("Bad 2nd boot block - reloading first\n");
            if( read_sector(0, buffer) != 0 )
               exit(1);
	 }
         check_msdos();
      }
#endif
      return;
   }
   if(!force) exit(2);
}

check_simpledos()
{
   int numclust = 0xFFFF;
   char * err = 0;
   check_msdos();

   /* Work out how many real clusters there are */
   if( dosflds[DOS_MAXSECT].value + 2 > 2 )
      numclust = ( dosflds[DOS_MAXSECT].value
                   - dosflds[DOS_RESV].value
                   - dosflds[DOS_NFAT].value * dosflds[DOS_FATLEN].value
                   - ((dosflds[DOS_NROOT].value+15)/16)
                 ) / dosflds[DOS_MAXSECT].value + 2;

   if( dosflds[DOS_NFAT].value > 2 )
      err = "Too many fat copies on disk";
   else if( dosflds[DOS_HIDDEN].value != 0 )
      err = "Dubious MSDOS floppy, it's got hidden sectors.";
   else if( dosflds[DOS_NROOT].value < 15 )
      err = "Root directory has unreasonable size.";
   else if( dosflds[DOS_SECT].value != 512 )
      err = "Drive sector size isn't 512 bytes sorry no-go.";
   else if( dosflds[DOS_HEADS].value != 2 )
      err = "Drive doesn't have two heads, this is required.";
   else if( numclust > 0xFF0 )
      err = "Filesystem has a 16 bit fat, only 12bits allowed.";
   else if( dosflds[DOS_RESV].value + dosflds[DOS_FATLEN].value > 
            dosflds[DOS_SPT].value )
      err = "The bootblock needs all of fat1 on the first track.";
   else
      return;

   fprintf(stderr, "ERROR: %s\n\n", err);
   print_super(buffer);
   if(!force) exit(2);
}

check_mbr()
{
   int i = 0;

   if( buffer[510] == 0x55 && buffer[511] == 0xAA )
      i = 512;

   for(; i<512; i++)
      if( buffer[i] )
         break;

   if( i != 512 )
   {
      if(force)
         fprintf(stderr, "That doesn't look like an MBR zapping\n");
      else
      {
         fprintf(stderr, "That doesn't look like an MBR, -f will zap\n");
         exit(1);
      }

      memset(buffer, '\0', 512);
   }
}

copy_mbr(mbr_data)
char * mbr_data;
{
   if( buffer[252] != 0xAA || buffer[253] != 0x55 )
      memcpy(buffer, mbr_data, 446);
   else
      memcpy(buffer, mbr_data, 254);
   memcpy(buffer+510, mbr_data+510, 2);
}

/**************************************************************************/

char boot_sector_2m_23_82[] = {
0xe9,0x7d,0x00,0x32,0x4d,0x2d,0x53,0x54,0x56,0x30,0x34,0x00,0x02,0x01,0x01,0x00,
0x02,0xe0,0x00,0xbc,0x0e,0xfa,0x0b,0x00,0x17,0x00,0x02,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x29,0x45,0xb8,0x25,0x51,0x4e,0x4f,0x20,0x4e,0x41,
0x4d,0x45,0x20,0x20,0x20,0x20,0x46,0x41,0x54,0x31,0x32,0x20,0x20,0x20,0x00,0x3f,
0x07,0x01,0x00,0x00,0x80,0x00,0x4c,0x00,0x61,0x00,0x79,0x00,0x13,0x46,0x01,0x02,
0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,
0x13,0x40,0x03,0x07,0x81,0x04,0x04,0x8c,0x01,0x04,0x97,0x05,0x04,0xa2,0x02,0x04,
0xad,0x06,0x03,0xb3,0x03,0x04,0xbe,0x07,0x02,0x04,0x04,0x04,0x04,0x04,0x03,0x02,
0xfa,0x33,0xc0,0x8e,0xd0,0xbc,0x00,0x7c,0xb8,0xc0,0x07,0x50,0x05,0x20,0x00,0x50,
0x07,0x1f,0x33,0xf6,0x33,0xff,0xb9,0x00,0x01,0xfc,0xf3,0xa5,0x8b,0x1e,0x44,0x00,
0x8d,0x47,0x26,0x06,0x50,0xcb,0xfb,0xbe,0x1a,0x01,0xe8,0xd9,0x00,0xbb,0x78,0x00,
0x36,0xc5,0x37,0x1e,0x56,0x33,0xff,0x36,0x89,0x3f,0x36,0x8c,0x47,0x02,0xb9,0x0b,
0x00,0xf3,0xa4,0x06,0x1f,0xa0,0x18,0x00,0x88,0x45,0xf9,0x33,0xc0,0x8e,0xc0,0xbb,
0x00,0x7c,0x26,0x89,0x87,0xfe,0x01,0xb8,0x01,0x02,0x8b,0x0e,0x16,0x00,0x83,0xc1,
0x02,0x33,0xd2,0x83,0xf9,0x0a,0x72,0x1f,0x51,0xcd,0x13,0x59,0x36,0x8b,0x1e,0x13,
0x04,0x83,0xeb,0x05,0xb8,0x40,0x00,0xf7,0xe3,0x8e,0xc0,0x53,0x33,0xdb,0xb8,0x05,
0x02,0x41,0x33,0xd2,0xcd,0x13,0x5b,0x36,0x8f,0x06,0x78,0x00,0x36,0x8f,0x06,0x7a,
0x00,0x26,0x81,0x3e,0xfe,0x09,0x55,0xaa,0x75,0x60,0x36,0x89,0x1e,0x13,0x04,0x06,
0xb4,0x08,0xb3,0x00,0xb2,0x00,0xcd,0x13,0x8a,0xc3,0xb4,0x00,0x80,0xfa,0x02,0x72,
0x0c,0x50,0xb4,0x08,0xb3,0x00,0xb2,0x01,0xcd,0x13,0x58,0x8a,0xe3,0x07,0x26,0x8c,
0x06,0xfe,0x09,0x26,0xff,0x1e,0xfc,0x09,0xb8,0x01,0x02,0x33,0xd2,0x8e,0xc2,0xb9,
0x01,0x00,0xbb,0x00,0x80,0x50,0xcd,0x13,0x58,0xbb,0x00,0x7c,0x06,0x53,0x26,0x81,
0x7f,0x03,0x32,0x4d,0x75,0x04,0xb2,0x80,0xcd,0x13,0x26,0x81,0x3e,0xfe,0x7d,0x55,
0xaa,0x75,0x03,0x33,0xd2,0xcb,0x22,0xd2,0x74,0xec,0xbe,0x2f,0x01,0xe8,0x06,0x00,
0xb4,0x00,0xcd,0x16,0xcd,0x19,0x03,0x36,0x44,0x00,0xfc,0xac,0x22,0xc0,0x74,0x09,
0xb4,0x0e,0xbb,0x07,0x00,0xcd,0x10,0xeb,0xf1,0xc3,0x0d,0x0a,0x32,0x4d,0x20,0x53,
0x75,0x70,0x65,0x72,0x42,0x4f,0x4f,0x54,0x20,0x32,0x2e,0x30,0x0d,0x0a,0x00,0x0d,
0x0a,0xad,0x4e,0x6f,0x20,0x62,0x6f,0x74,0x61,0x62,0x6c,0x65,0x21,0x0d,0x0a,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x4d,0x61,0x64,0x65,0x20,0x69,0x6e,0x20,0x53,0x70,0x61,0x69,0x6e,0x00,0x55,0xaa
};

char boot_sector_2m_22_82[] = {
0xe9,0x6e,0x00,0x32,0x4d,0x2d,0x53,0x54,0x56,0x30,0x38,0x00,0x02,0x01,0x01,0x00,
0x02,0xe0,0x00,0x18,0x0e,0xfa,0x0b,0x00,0x16,0x00,0x02,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x29,0xcc,0x9b,0xe1,0xd4,0x4e,0x4f,0x20,0x4e,0x41,
0x4d,0x45,0x20,0x20,0x20,0x20,0x46,0x41,0x54,0x31,0x32,0x20,0x20,0x20,0x00,0x04,
0x07,0x00,0x00,0x00,0x71,0x00,0x4c,0x00,0x61,0x00,0x66,0x00,0x13,0x46,0x01,0x02,
0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,
0x13,0x0b,0x28,0x03,0x01,0x02,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
0x03,0xfa,0x33,0xc0,0x8e,0xd0,0xbc,0x00,0x7c,0xb8,0xc0,0x07,0x50,0x05,0x20,0x00,
0x50,0x07,0x1f,0x33,0xf6,0x33,0xff,0xb9,0x00,0x01,0xfc,0xf3,0xa5,0x8b,0x1e,0x44,
0x00,0x8d,0x47,0x26,0x06,0x50,0xcb,0xfb,0xbe,0x1a,0x01,0xe8,0xd9,0x00,0xbb,0x78,
0x00,0x36,0xc5,0x37,0x1e,0x56,0x33,0xff,0x36,0x89,0x3f,0x36,0x8c,0x47,0x02,0xb9,
0x0b,0x00,0xf3,0xa4,0x06,0x1f,0xa0,0x18,0x00,0x88,0x45,0xf9,0x33,0xc0,0x8e,0xc0,
0xbb,0x00,0x7c,0x26,0x89,0x87,0xfe,0x01,0xb8,0x01,0x02,0x8b,0x0e,0x16,0x00,0x83,
0xc1,0x02,0x33,0xd2,0x83,0xf9,0x0a,0x72,0x1f,0x51,0xcd,0x13,0x59,0x36,0x8b,0x1e,
0x13,0x04,0x83,0xeb,0x05,0xb8,0x40,0x00,0xf7,0xe3,0x8e,0xc0,0x53,0x33,0xdb,0xb8,
0x05,0x02,0x41,0x33,0xd2,0xcd,0x13,0x5b,0x36,0x8f,0x06,0x78,0x00,0x36,0x8f,0x06,
0x7a,0x00,0x26,0x81,0x3e,0xfe,0x09,0x55,0xaa,0x75,0x60,0x36,0x89,0x1e,0x13,0x04,
0x06,0xb4,0x08,0xb3,0x00,0xb2,0x00,0xcd,0x13,0x8a,0xc3,0xb4,0x00,0x80,0xfa,0x02,
0x72,0x0c,0x50,0xb4,0x08,0xb3,0x00,0xb2,0x01,0xcd,0x13,0x58,0x8a,0xe3,0x07,0x26,
0x8c,0x06,0xfe,0x09,0x26,0xff,0x1e,0xfc,0x09,0xb8,0x01,0x02,0x33,0xd2,0x8e,0xc2,
0xb9,0x01,0x00,0xbb,0x00,0x80,0x50,0xcd,0x13,0x58,0xbb,0x00,0x7c,0x06,0x53,0x26,
0x81,0x7f,0x03,0x32,0x4d,0x75,0x04,0xb2,0x80,0xcd,0x13,0x26,0x81,0x3e,0xfe,0x7d,
0x55,0xaa,0x75,0x03,0x33,0xd2,0xcb,0x22,0xd2,0x74,0xec,0xbe,0x2f,0x01,0xe8,0x06,
0x00,0xb4,0x00,0xcd,0x16,0xcd,0x19,0x03,0x36,0x44,0x00,0xfc,0xac,0x22,0xc0,0x74,
0x09,0xb4,0x0e,0xbb,0x07,0x00,0xcd,0x10,0xeb,0xf1,0xc3,0x0d,0x0a,0x32,0x4d,0x20,
0x53,0x75,0x70,0x65,0x72,0x42,0x4f,0x4f,0x54,0x20,0x32,0x2e,0x30,0x0d,0x0a,0x00,
0x0d,0x0a,0xad,0x4e,0x6f,0x20,0x62,0x6f,0x74,0x61,0x62,0x6c,0x65,0x21,0x0d,0x0a,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x4d,0x61,0x64,0x65,0x20,0x69,0x6e,0x20,0x53,0x70,0x61,0x69,0x6e,0x00,0x55,0xaa
};


char program_2m_vsn_20[] = {
0x2b,0x00,0x43,0x00,0x32,0x30,0x32,0x4d,0x2d,0x53,0x54,0x56,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0a,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46,0x4a,0x42,0x00,0x00,0x01,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xfb,0xfc,0x9c,0x56,0x80,
0xfa,0x02,0x73,0x3d,0xe8,0x41,0x00,0x2e,0x80,0x3c,0x02,0x74,0x04,0x2e,0x80,0x3c,
0x04,0x72,0x2e,0x80,0xfc,0x02,0x72,0x29,0x80,0xfc,0x05,0x77,0x24,0x75,0x05,0xe8,
0x36,0x00,0xeb,0x1d,0xe8,0x85,0x00,0x73,0x09,0x5e,0x9d,0xf9,0xb8,0x00,0x06,0xca,
0x02,0x00,0x2e,0x80,0x7c,0x01,0x00,0x74,0x08,0x5e,0x9d,0xe8,0x3c,0x02,0xca,0x02,
0x00,0x5e,0x9d,0x2e,0xff,0x2e,0xfc,0x09,0x9c,0x53,0x8a,0xda,0xb7,0x00,0xd1,0xe3,
0x2e,0x8b,0xb7,0x00,0x00,0x5b,0x9d,0xc3,0x60,0xe8,0xec,0xff,0xb0,0x01,0x72,0x02,
0xb0,0x00,0x2e,0x88,0x44,0x01,0x61,0xc3,0x50,0xa0,0x12,0x00,0x0a,0x06,0x11,0x00,
0x58,0xc3,0x60,0x1e,0x6a,0x40,0x1f,0xb0,0x01,0x8a,0xca,0xd2,0xe0,0x84,0x06,0x3f,
0x00,0x75,0x04,0xf8,0xe8,0xef,0x05,0x8a,0xe2,0xc0,0xe4,0x04,0x0a,0xe0,0xc0,0xe0,
0x04,0x0c,0x0c,0x0a,0xc2,0xba,0xf2,0x03,0xfa,0x88,0x26,0x3f,0x00,0xee,0x83,0xc2,
0x05,0xeb,0x00,0xeb,0x00,0xec,0xfb,0xa8,0x80,0x1f,0x61,0xc3,0x60,0xe8,0x98,0xff,
0x2e,0x80,0x7c,0x02,0x01,0x2e,0xc6,0x44,0x02,0x00,0x74,0x08,0xe8,0xb3,0xff,0x75,
0x03,0x61,0xf8,0xc3,0xf8,0xe8,0x90,0xff,0x1e,0x06,0xbb,0x90,0x00,0x02,0xda,0x6a,
0x40,0x1f,0x80,0x27,0xef,0x0e,0x0e,0x1f,0x07,0x88,0x16,0x0c,0x00,0xf9,0xe8,0x29,
0x05,0xc6,0x06,0x11,0x00,0x01,0xc6,0x06,0x12,0x00,0x00,0xe8,0xa1,0x05,0xfe,0x0e,
0x11,0x00,0xe8,0x9a,0x05,0xf8,0xe8,0x7d,0x05,0xe8,0x76,0xff,0x74,0x07,0xc6,0x44,
0x02,0x01,0xf8,0xeb,0x68,0x1e,0x6a,0x40,0x1f,0xc6,0x06,0x41,0x00,0x06,0x1f,0xc6,
0x06,0x1b,0x00,0xff,0xc6,0x44,0x08,0x14,0xb9,0x03,0x00,0x51,0x83,0xf9,0x02,0xe8,
0xe8,0x04,0xc6,0x44,0x06,0x00,0xc6,0x06,0x11,0x00,0x00,0xc6,0x06,0x12,0x00,0x00,
0xc6,0x06,0x13,0x00,0x01,0xc6,0x06,0x16,0x00,0x00,0xc6,0x06,0x17,0x00,0x01,0xc6,
0x06,0x27,0x00,0x46,0x8b,0x3e,0x19,0x00,0xe8,0x7f,0x02,0x75,0x0b,0x59,0x8b,0x1e,
0x19,0x00,0xe8,0x1c,0x00,0xf8,0xeb,0x14,0x8a,0x44,0x06,0x40,0x3c,0x03,0x77,0x05,
0x88,0x44,0x06,0xeb,0xc1,0xc6,0x44,0x06,0x00,0x59,0xe2,0xaf,0xf9,0x07,0x1f,0x61,
0xc3,0x60,0xe8,0x88,0x00,0x72,0x5c,0x88,0x44,0x05,0x88,0x4c,0x03,0x8a,0x16,0x0c,
0x00,0xf9,0xe8,0xd3,0xfe,0x26,0x8a,0x47,0x16,0x88,0x44,0x17,0x26,0x8a,0x4f,0x41,
0x88,0x4c,0x04,0x26,0x8b,0x47,0x42,0x89,0x44,0x06,0x26,0x8a,0x47,0x18,0x88,0x44,
0x09,0x26,0x8b,0x7f,0x48,0x26,0x8a,0x41,0x01,0x8a,0xe0,0x22,0xc9,0x74,0x0a,0x80,
0xc4,0xbe,0xb0,0x0b,0xf6,0xe4,0x2d,0x3e,0x08,0xd0,0xe8,0x88,0x44,0x08,0xb9,0x0d,
0x00,0x26,0x8b,0x7f,0x4a,0x03,0xfb,0x8d,0x5c,0x0a,0x26,0x8a,0x05,0x88,0x07,0x43,
0x47,0xe2,0xf7,0x8a,0x44,0x06,0xc0,0xe0,0x06,0x0c,0x17,0x80,0x3c,0x02,0x77,0x0a,
0x24,0xf8,0x0c,0x05,0xa8,0x40,0x74,0x02,0x34,0x21,0x1e,0xbb,0x90,0x00,0x02,0x1e,
0x0c,0x00,0x6a,0x40,0x1f,0x80,0x27,0x08,0x08,0x07,0x1f,0x61,0xc3,0x56,0x57,0x8d,
0x7f,0x03,0xbe,0x06,0x00,0xb9,0x06,0x00,0xf3,0xa6,0xf9,0x75,0x19,0x33,0xc0,0x26,
0x8a,0x4f,0x40,0x80,0xf9,0x06,0x72,0x0d,0x26,0x8b,0x7f,0x44,0x4f,0x26,0x02,0x01,
0x83,0xff,0x3f,0x77,0xf7,0xf8,0x5f,0x5e,0xc3,0x50,0x73,0x37,0x80,0x3e,0x1f,0x00,
0x00,0x75,0x2f,0xa0,0x21,0x00,0xd0,0xe0,0xb4,0x04,0x72,0x22,0xc0,0xe0,0x02,0xb4,
0x10,0x72,0x1b,0xd0,0xe0,0xb4,0x08,0x72,0x15,0xc0,0xe0,0x02,0xb4,0x04,0x72,0x0e,
0xd0,0xe0,0xb4,0x03,0x72,0x08,0xd0,0xe0,0xb4,0x02,0x72,0x02,0xb4,0x20,0x08,0x26,
0x1f,0x00,0xf9,0x58,0xc3,0x9c,0x60,0x06,0x6a,0x40,0x07,0xbf,0x41,0x00,0xbe,0x1f,
0x00,0xb9,0x04,0x00,0xf3,0xa5,0x07,0x61,0x9d,0xc3,0x1e,0x60,0x0e,0x1f,0x88,0x16,
0x0c,0x00,0xe8,0xc3,0xfd,0x80,0x7c,0x05,0x00,0x74,0x08,0xc6,0x06,0x1f,0x00,0x40,
0xe9,0xbb,0x00,0x50,0xb4,0x00,0xa3,0x0d,0x00,0x8a,0xc5,0xd0,0xe0,0x8a,0xd6,0x80,
0xe6,0x7f,0x02,0xc6,0xf6,0x64,0x09,0x02,0xc1,0x80,0xd4,0x00,0x48,0xa3,0x0f,0x00,
0x8b,0xfb,0x5b,0x8a,0xdf,0xb7,0x00,0x8a,0x8f,0x26,0x00,0x88,0x0e,0x27,0x00,0xd0,
0xe2,0x72,0x73,0x23,0xc0,0x75,0x2c,0x80,0x7c,0x03,0x07,0x72,0x19,0x8a,0x44,0x17,
0x40,0xb9,0x01,0x00,0xe8,0x9b,0x00,0x75,0x6e,0xff,0x0e,0x0d,0x00,0xff,0x06,0x0f,
0x00,0xa1,0x0f,0x00,0xeb,0x0d,0x80,0x3e,0x27,0x00,0x4a,0x75,0x06,0x81,0xc7,0x00,
0x02,0xeb,0xe6,0x8a,0x4c,0x17,0xb5,0x00,0x3b,0xc1,0x77,0x0f,0xe8,0x5d,0x00,0xe8,
0x70,0x00,0x75,0x43,0x83,0x3e,0x0d,0x00,0x00,0x74,0x3c,0xa1,0x0f,0x00,0x8a,0x4c,
0x17,0xb5,0x00,0xd1,0xe1,0x3b,0xc1,0x77,0x1d,0xe8,0x40,0x00,0x80,0x3e,0x27,0x00,
0x4a,0x75,0x07,0xc1,0xe1,0x09,0x03,0xf9,0xeb,0x0c,0x8a,0x54,0x17,0xb6,0x00,0x2b,
0xc2,0xe8,0x3e,0x00,0x75,0x11,0x83,0x3e,0x0d,0x00,0x00,0x74,0x0a,0xa1,0x0f,0x00,
0x8b,0x0e,0x0d,0x00,0xe8,0x2b,0x00,0xf8,0xe8,0x2b,0x03,0xe8,0x17,0xff,0x61,0x8a,
0x26,0x1f,0x00,0x1f,0x22,0xe4,0x74,0x03,0xf9,0xb0,0x00,0xc3,0x2b,0xc8,0x41,0x3b,
0x0e,0x0d,0x00,0x76,0x04,0x8b,0x0e,0x0d,0x00,0x29,0x0e,0x0d,0x00,0x01,0x0e,0x0f,
0x00,0xc3,0x8b,0xd8,0x88,0x0e,0x17,0x00,0xf6,0x74,0x09,0xfe,0xc4,0x88,0x26,0x13,
0x00,0xd0,0xe8,0xa2,0x11,0x00,0xd0,0xd0,0x24,0x01,0xa2,0x12,0x00,0xa0,0x13,0x00,
0x02,0x06,0x17,0x00,0x72,0x06,0x48,0x3a,0x44,0x09,0x76,0x07,0xc6,0x06,0x1f,0x00,
0x04,0xeb,0x77,0x8a,0xc4,0x98,0xe8,0xbf,0xfc,0x74,0x18,0x8d,0x5c,0x09,0x48,0x43,
0xfe,0xc4,0x8a,0x0f,0x80,0xe9,0x02,0xb5,0x01,0xd2,0xe5,0x2a,0xc5,0x73,0xf0,0x02,
0xc5,0x86,0xe0,0xa2,0x13,0x00,0x88,0x26,0x16,0x00,0xe8,0xe8,0x01,0xb4,0x00,0x88,
0x26,0x15,0x00,0xe8,0x92,0xfc,0x75,0x09,0xa0,0x17,0x00,0x88,0x26,0x17,0x00,0xeb,
0x28,0x38,0x64,0x04,0x75,0x28,0x38,0x26,0x16,0x00,0x74,0x05,0xe8,0x31,0x00,0x72,
0x29,0x38,0x26,0x17,0x00,0x74,0x23,0xe8,0x9b,0x01,0x8a,0xc8,0xa0,0x17,0x00,0xf6,
0xf1,0x22,0xc0,0x74,0x09,0x88,0x26,0x17,0x00,0xe8,0x82,0x00,0x72,0x0c,0x80,0x3e,
0x17,0x00,0x00,0x74,0x05,0xe8,0x08,0x00,0x73,0xf4,0x80,0x3e,0x1f,0x00,0x00,0xc3,
0x50,0x80,0x3e,0x27,0x00,0x4a,0x74,0x16,0x80,0x3e,0x27,0x00,0x42,0x74,0x3b,0xe8,
0xbb,0x00,0x73,0x05,0xe8,0xdb,0x00,0x72,0x48,0xe8,0x6f,0x00,0xeb,0x43,0x80,0x3e,
0x16,0x00,0x00,0x75,0x09,0xe8,0x4d,0x01,0x38,0x06,0x17,0x00,0x73,0x14,0xe8,0x9c,
0x00,0x73,0x0f,0xc6,0x06,0x27,0x00,0x46,0xe8,0xb7,0x00,0xc6,0x06,0x27,0x00,0x4a,
0x72,0x1f,0xe8,0x46,0x00,0xe8,0xaa,0x00,0xeb,0x17,0x53,0x8a,0x1e,0x16,0x00,0xe8,
0x23,0x01,0xfe,0x0e,0x17,0x00,0x74,0x05,0x43,0x3a,0xd8,0x72,0xf5,0x5b,0xe8,0x91,
0x00,0x9c,0xfe,0x06,0x13,0x00,0xc6,0x06,0x16,0x00,0x00,0x9d,0x58,0xc3,0x50,0x22,
0xc0,0x74,0x16,0x8a,0x26,0x13,0x00,0x88,0x26,0x14,0x00,0x02,0xc4,0x48,0xa2,0x15,
0x00,0xfe,0xc0,0xe8,0x6c,0x00,0xa2,0x13,0x00,0x58,0xc3,0x50,0x53,0x51,0x56,0x8a,
0x1e,0x16,0x00,0xe8,0xdf,0x00,0x53,0xc1,0xe3,0x09,0x03,0x1e,0x19,0x00,0x8b,0xf3,
0xb9,0x00,0x01,0xe8,0x16,0x00,0xf3,0xa5,0xe8,0x11,0x00,0x5b,0xfe,0x0e,0x17,0x00,
0x74,0x05,0x43,0x3a,0xd8,0x72,0xdf,0x5e,0x59,0x5b,0x58,0xc3,0x2e,0x80,0x3e,0x27,
0x00,0x4a,0x74,0x02,0xf8,0xc3,0x87,0xf7,0x06,0x1e,0x07,0x1f,0xc3,0x50,0xa0,0x1b,
0x00,0x3a,0x06,0x0c,0x00,0x75,0x18,0xa0,0x11,0x00,0x8a,0x26,0x12,0x00,0x3b,0x06,
0x1c,0x00,0x75,0x0b,0xa0,0x1e,0x00,0x3a,0x06,0x13,0x00,0x75,0x02,0x58,0xc3,0xf9,
0x58,0xc3,0x50,0x53,0xe8,0x78,0x01,0x73,0x0f,0x80,0x3e,0x1f,0x00,0x00,0x75,0x05,
0x80,0x0e,0x1f,0x00,0x40,0xf9,0xeb,0x6a,0xe8,0x3d,0xfb,0xb0,0x02,0x74,0x0d,0x8d,
0x5c,0x0a,0x02,0x1e,0x13,0x00,0x80,0xd7,0x00,0x8a,0x47,0xff,0xa2,0x18,0x00,0x80,
0x3e,0x15,0x00,0x00,0x74,0x0b,0xe8,0x5c,0x02,0xc6,0x06,0x15,0x00,0x00,0x9c,0xeb,
0x3d,0x06,0x57,0x0e,0x07,0x8b,0x3e,0x19,0x00,0xa0,0x13,0x00,0xa2,0x14,0x00,0xa2,
0x15,0x00,0xe8,0x40,0x02,0xc6,0x06,0x15,0x00,0x00,0x5f,0x07,0x9c,0xb0,0xff,0x72,
0x0a,0x80,0x3e,0x27,0x00,0x42,0x74,0x16,0xa0,0x0c,0x00,0xa2,0x1b,0x00,0xa0,0x11,
0x00,0x8a,0x26,0x12,0x00,0xa3,0x1c,0x00,0xa0,0x13,0x00,0xa2,0x1e,0x00,0x9d,0xe8,
0x97,0xfc,0x5b,0x58,0xc3,0xe8,0xd0,0xfa,0xb0,0x01,0x74,0x18,0x53,0x51,0x8d,0x5c,
0x0a,0x02,0x1e,0x13,0x00,0x80,0xd7,0x00,0x8a,0x4f,0xff,0x80,0xe9,0x02,0xb0,0x01,
0xd2,0xe0,0x59,0x5b,0xc3,0x60,0x1e,0xbb,0x40,0x00,0x53,0x1f,0xb5,0xed,0xfa,0x2e,
0x8a,0x0e,0x0c,0x00,0xb0,0x01,0xd2,0xe0,0x84,0x47,0xff,0x74,0x04,0x38,0x2f,0x76,
0x33,0x08,0x47,0xff,0x80,0x67,0xff,0xcf,0x8a,0xc1,0xc0,0xe0,0x04,0x08,0x47,0xff,
0xc6,0x07,0xff,0xfb,0xba,0xf2,0x03,0x80,0xc1,0x04,0xb0,0x01,0xd2,0xe0,0x2e,0x0a,
0x06,0x0c,0x00,0x0c,0x0c,0xee,0xb8,0xfd,0x90,0xf8,0xcd,0x15,0x72,0x06,0xb8,0xe8,
0x03,0xe8,0x48,0x03,0x88,0x2f,0xfb,0x1f,0x61,0xc3,0x60,0xe8,0x68,0x00,0x8a,0x0e,
0x0c,0x00,0x8a,0xc1,0xc0,0xe0,0x02,0x0c,0x01,0xd2,0xe0,0x1e,0x6a,0x40,0x1f,0xfa,
0xa2,0x3f,0x00,0x80,0x26,0x3e,0x00,0x70,0x1f,0xc0,0xe0,0x04,0x0a,0xc1,0x0c,0x08,
0xba,0xf2,0x03,0xee,0xe8,0x08,0x03,0x0c,0x04,0xee,0xe8,0x1a,0x02,0xb0,0x08,0xe8,
0xc3,0x02,0xe8,0x82,0x02,0xe8,0x7f,0x02,0xe8,0x02,0x00,0x61,0xc3,0x50,0x1e,0x6a,
0x40,0x1f,0x8a,0x26,0x8b,0x00,0x1f,0xb0,0x03,0xe8,0xa9,0x02,0xb0,0xbf,0x80,0xe4,
0xc0,0x74,0x09,0xb0,0xaf,0x80,0xfc,0xc0,0x74,0x02,0xb0,0xdf,0xe8,0x96,0x02,0xb0,
0x02,0xe8,0x91,0x02,0x58,0xc3,0x60,0x1e,0xb0,0xff,0x72,0x0a,0x6a,0x00,0x1f,0xc5,
0x1e,0x78,0x00,0x8a,0x47,0x02,0x6a,0x40,0x1f,0xa2,0x40,0x00,0x1f,0x61,0xc3,0x60,
0xe8,0x87,0x00,0xe8,0xb7,0xff,0xb4,0x01,0x8a,0x0e,0x0c,0x00,0xd2,0xe4,0x1e,0x6a,
0x40,0x1f,0x84,0x26,0x3e,0x00,0x1f,0x75,0x05,0xe8,0xa6,0x00,0x72,0x69,0xbb,0x94,
0x00,0x02,0x1e,0x0c,0x00,0xa0,0x11,0x00,0x1e,0x6a,0x40,0x1f,0x08,0x26,0x3e,0x00,
0x8a,0x26,0x41,0x00,0x3a,0x07,0x88,0x07,0x1f,0x75,0x05,0x80,0xfc,0x40,0x75,0x44,
0xb0,0x0f,0xe8,0x30,0x02,0x72,0x40,0xa0,0x12,0x00,0xc0,0xe0,0x02,0x0a,0x06,0x0c,
0x00,0xe8,0x21,0x02,0xa0,0x11,0x00,0xe8,0x1b,0x02,0xe8,0x6a,0x01,0x72,0x28,0xb0,
0x08,0xe8,0x11,0x02,0x72,0x21,0xe8,0xce,0x01,0x72,0x1c,0x8a,0xe0,0xe8,0xc7,0x01,
0xf6,0xc4,0xc0,0x75,0x12,0xb0,0x0f,0x80,0x3e,0x27,0x00,0x4a,0x74,0x02,0xb0,0x01,
0x98,0xe8,0x38,0x02,0x61,0xf8,0xc3,0x61,0xf9,0xc3,0x60,0xe8,0x4a,0xf9,0x8b,0x44,
0x06,0x74,0x02,0x8a,0xc4,0x1e,0x6a,0x40,0x1f,0x8a,0x26,0x8b,0x00,0xc0,0xec,0x06,
0x3a,0xc4,0x74,0x10,0xba,0xf7,0x03,0xee,0xc0,0xe0,0x06,0x80,0x26,0x8b,0x00,0x3f,
0x08,0x06,0x8b,0x00,0x1f,0xbf,0x1f,0x00,0xb9,0x08,0x00,0x88,0x2d,0x47,0xe2,0xfb,
0x61,0xc3,0x60,0xbb,0x94,0x00,0x02,0x1e,0x0c,0x00,0x1e,0x6a,0x40,0x1f,0x88,0x3f,
0x1f,0xb9,0x02,0x00,0xb0,0x07,0xe8,0x9c,0x01,0x72,0x35,0xa0,0x12,0x00,0xc0,0xe0,
0x02,0x0a,0x06,0x0c,0x00,0xe8,0x8d,0x01,0x72,0x26,0xe8,0xda,0x00,0x72,0x21,0xb0,
0x08,0xe8,0x81,0x01,0x72,0x1a,0xe8,0x3e,0x01,0x72,0x15,0x8a,0xe0,0xe8,0x37,0x01,
0x80,0xf4,0x20,0xf6,0xc4,0xf0,0x75,0x08,0xb8,0x01,0x00,0xe8,0xae,0x01,0xeb,0x03,
0xe2,0xc2,0xf9,0x61,0xc3,0x50,0x53,0x51,0x52,0x8a,0x0e,0x18,0x00,0xb5,0x00,0xf9,
0xd2,0xd5,0xb1,0x00,0xa0,0x15,0x00,0x2a,0x06,0x14,0x00,0x40,0x98,0xf7,0xe1,0x8b,
0xd0,0x8b,0xc8,0x49,0x8c,0xc0,0xe8,0x72,0x00,0x72,0x6a,0xa0,0x27,0x00,0xe8,0xb7,
0x00,0x3c,0x4a,0xb0,0xc5,0x74,0x02,0xb0,0xe6,0xe8,0x29,0x01,0x72,0x57,0xa0,0x12,
0x00,0xc0,0xe0,0x02,0x0a,0x06,0x0c,0x00,0xe8,0x1a,0x01,0xa0,0x11,0x00,0xe8,0x14,
0x01,0xa0,0x12,0x00,0xe8,0x0e,0x01,0xa0,0x14,0x00,0xe8,0x08,0x01,0xa0,0x18,0x00,
0xe8,0x02,0x01,0xa0,0x15,0x00,0xe8,0xfc,0x00,0x8a,0x44,0x08,0xe8,0xf6,0x00,0xb0,
0x80,0xe8,0xf1,0x00,0xe8,0x40,0x00,0x9c,0xbb,0x20,0x00,0xb9,0x07,0x00,0xe8,0xa6,
0x00,0x88,0x07,0x43,0xe2,0xf8,0x9d,0x72,0x0c,0xf6,0x06,0x20,0x00,0xc0,0x75,0x05,
0x03,0xfa,0xf8,0xeb,0x01,0xf9,0x5a,0x59,0x5b,0x58,0xc3,0x52,0xbb,0x10,0x00,0xf7,
0xe3,0x03,0xc7,0x83,0xd2,0x00,0x8b,0xd8,0x8a,0xe2,0x8b,0xd1,0x03,0xd3,0x73,0x05,
0xc6,0x06,0x1f,0x00,0x09,0x5a,0xc3,0xfb,0x60,0x1e,0x6a,0x40,0x1f,0xb8,0x01,0x90,
0xf8,0xcd,0x15,0xba,0x80,0x02,0xbb,0x3e,0x00,0x72,0x0f,0x33,0xc9,0x84,0x17,0x75,
0x0f,0xe8,0xf6,0x00,0xe2,0xf7,0xfe,0xce,0x75,0xf1,0x2e,0x08,0x16,0x1f,0x00,0xf9,
0x9c,0x80,0x27,0x7f,0x9d,0x1f,0x61,0xc3,0x50,0xfa,0xe6,0x0b,0xb0,0x00,0xeb,0x00,
0xeb,0x00,0xe6,0x0c,0x8a,0xc3,0xeb,0x00,0xeb,0x00,0xe6,0x04,0x8a,0xc7,0xeb,0x00,
0xeb,0x00,0xe6,0x04,0xeb,0x00,0xeb,0x00,0x8a,0xc4,0xe6,0x81,0x8a,0xc1,0xeb,0x00,
0xeb,0x00,0xe6,0x05,0x8a,0xc5,0xeb,0x00,0xeb,0x00,0xe6,0x05,0xfb,0xb0,0x02,0xeb,
0x00,0xeb,0x00,0xe6,0x0a,0x58,0xc3,0x51,0x52,0x50,0xe8,0x72,0x00,0xba,0xf4,0x03,
0xb9,0x85,0x00,0xeb,0x00,0xeb,0x00,0xec,0x24,0xc0,0x3c,0xc0,0x74,0x1c,0xeb,0x00,
0xeb,0x00,0xe4,0x61,0x24,0x10,0x3a,0xc4,0x74,0xe9,0x8a,0xe0,0xe2,0xe5,0x58,0x5a,
0x59,0x80,0x0e,0x1f,0x00,0x80,0xb0,0x00,0xf9,0xc3,0x58,0x42,0xeb,0x00,0xeb,0x00,
0xec,0x5a,0x59,0xf8,0xc3,0x51,0x52,0x50,0xe8,0x34,0x00,0xba,0xf4,0x03,0xb9,0x85,
0x00,0xeb,0x00,0xeb,0x00,0xec,0xa8,0x80,0x75,0x1a,0xeb,0x00,0xeb,0x00,0xe4,0x61,
0x24,0x10,0x3a,0xc4,0x74,0xeb,0x8a,0xe0,0xe2,0xe7,0x58,0x5a,0x59,0x80,0x0e,0x1f,
0x00,0x80,0xf9,0xc3,0x42,0x58,0xeb,0x00,0xeb,0x00,0xee,0x5a,0x59,0xf8,0xc3,0x50,
0x51,0xb9,0x04,0x00,0xe8,0x23,0x00,0xe2,0xfb,0x59,0x58,0xc3,0x9c,0x60,0xba,0x4a,
0x42,0xf7,0xe2,0x8a,0xcc,0x8a,0xea,0x8a,0xd6,0xb6,0x00,0xe8,0x0c,0x00,0xe2,0xfb,
0x23,0xd2,0x74,0x03,0x4a,0xeb,0xf4,0x61,0x9d,0xc3,0xeb,0x00,0xeb,0x00,0xe4,0x61,
0x24,0x10,0x3a,0xc4,0x74,0xf4,0x8a,0xe0,0xc3,0x1e,0x16,0x1f,0x26,0xa2,0x2b,0x00,
0x26,0x88,0x26,0x43,0x00,0xbf,0xfc,0x09,0xbe,0x4c,0x00,0xfc,0xfa,0xa5,0xa5,0xc7,
0x44,0xfc,0x5b,0x00,0x8c,0x44,0xfe,0xfb,0x1f,0xcb,0x00,0x00,0xd9,0x09,0x55,0xaa
};

do_2m_write()
{
   int i;

   if( read_sector(bs_offset+1, buffer+512) != 0 )
      exit(1);

   if( memcmp(buffer+512, program_2m_vsn_20, 16) == 0 )
   {
      /* Seems to be properly formatted already */

      write_sector(bs_offset, buffer);
      return;
   }
   else if( disk_trck != 82 || disk_sect != 22 )
   {
      fprintf(stderr, "To be bootable a 2M disk must be 22 sectors 82 tracks or formatted with 2m20.\n");
      if( !force ) exit(1);
      fprintf(stderr, "But I'll try it\n");
   }
   write_sector(bs_offset, buffer);

   /* This needs to be altered to allow for the disk format description to
      be copied from the old boot sector */

   for(i=0; i<sysboot_dosfs_stat; i++)
      buffer[i] = boot_sector_2m_22_82[i];
   for(i=sysboot_codestart; i<512; i++)
      buffer[i] = boot_sector_2m_22_82[i];

   write_sector(0, buffer);

   for(i=0; i<sizeof(program_2m_vsn_20); i+=512)
   {
      write_sector(bs_offset+i/512+1, program_2m_vsn_20+i);
   }
}
