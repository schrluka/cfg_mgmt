/******************************************************************************************************************************
*
*   CFG_MGMT  -  Configuration Variable Management for AMP using RPMSG
*   Linux user space applications and cli tools
*
*   Copyright (c) 2015 Lukas Schrittwieser
*
*   Permission is hereby granted, free of charge, to any person obtaining a copy
*   of this software and associated documentation files (the "Software"), to deal
*   in the Software without restriction, including without limitation the rights
*   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
*   copies of the Software, and to permit persons to whom the Software is
*   furnished to do so, subject to the following conditions:
*
*   The above copyright notice and this permission notice shall be included in
*   all copies or substantial portions of the Software.
*
*   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
*   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
*   THE SOFTWARE.
*
*******************************************************************************************************************************
*
*   clist.c
*
*   Top Level File, implements an cli application which lists all variable names, their values and descriptions
*
******************************************************************************************************************************/

#include <stdio.h>
#include <dirent.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define DFLT_PATH   "/debug/cfg_mgmt"

// max string length for variable names (will truncate)
#define MAX_NAME_LEN        40

// max string length of int32 printed in human readable form
#define MAX_NUMERIC_LEN     12

#define MAX_DESC_LEN        200



/******************************************************************************************************************************
*   P R O T O T Y P E S
*/

int show(const char* cfg_mgmt_path);

void puts_pad(const char* str, int len);

void load(DIR* dirp, int num, const char* cfg_mgmt_path, char** names, char** values,
            char** minima, char** maxima, char** descs);

int load_file(const char* fn, char* buf, int n);

void help();



/******************************************************************************************************************************
*   I M P L E M E N T A T I O N
*/

int main (int argc, char **argv)
{
    char* cfg_mgmt_path = DFLT_PATH;  // location where debugfs is mounted
    int index;
    int c;

    opterr = 0;
    // parse cli input for options
    while ((c = getopt (argc, argv, "hd:")) != -1) {
        switch (c) {
        case 'd':
            cfg_mgmt_path = optarg;
            break;
        case 'h':
            help();
        case '?':
            if (optopt == 'd') {
                fprintf (stderr, "Option -%c requires an argument.\n", optopt);
                cfg_mgmt_path = NULL;
            } else if (isprint (optopt))
              fprintf (stderr, "Unknown option `-%c'.\n", optopt);
            else
              fprintf (stderr, "Unknown option character `\\x%x'.\n", optopt);
            return 1;
        default:
            abort();
        }
    }
    if (cfg_mgmt_path == NULL) {
        return -2;
    }
    show(cfg_mgmt_path);
}



int show(const char* cfg_mgmt_path)
{
    // arrays of strings to be shown
    char** names;
    char** values;
    char** minima;
    char** maxima;
    char** descs;
    int num;    // number of variables (ie number of entries in each array)
    const int fn_buf_len = 256;
    int i, ret;

    // parse directory
    DIR *dirp;
    struct dirent *ep;

    // check how many files we have
    char fn[fn_buf_len];
    snprintf(fn, fn_buf_len, "%s/val", cfg_mgmt_path);
    dirp = opendir(fn);
    if (!dirp) {
        fprintf(stderr, "%s: can't open directory %s: %d\n",__func__, fn, errno);
        return -errno;
    }
    num = 0;
    while (ep=readdir(dirp)) {
        // ignore all files starting with a .
        if (ep->d_name[0] != '.')
            num++;
    }
    rewinddir(dirp);    // reset directory stream

    // get memory for the pointers
    names = malloc(num*sizeof(char*));
    values = malloc(num*sizeof(char*));
    minima = malloc(num*sizeof(char*));
    maxima = malloc(num*sizeof(char*));
    descs = malloc(num*sizeof(char*));
    // load all file contents
    load(dirp, num, cfg_mgmt_path, names, values, minima, maxima, descs);

	// first task: find max length of the columns (for pretty formatting)
    int nName=4, nVal=5, nMin=7, nMax=7;	// max size of the columns, start values represent header of table
	for (i=0; i<num; i++) {
		// load variable struct
		int l = strlen(names[i]);
		if (l>nName)
			nName = l;
		l = strlen(values[i]);
		if (l > nVal)
			nVal = l;
		l = strlen(minima[i]);
		if (l > nMin)
			nMin = l;
		l = strlen(maxima[i]);
		if (l > nMax)
			nMax = l;
		i++;
	}

	// add one to all length to include an extra space for the longest name
	nName++;
	nVal++;
	nMin++;
	nMax++;
    // print the header
    puts_pad("Name", nName);
    puts_pad("Value", nVal);
    puts_pad("Minimum,", nMin);
    puts_pad("Maximum", nMax);
    puts("Description");
    // show all variables
    for (i=0; i<num; i++) {
        puts_pad(names[i], nName);
        puts_pad(values[i], nVal);
        puts_pad(minima[i], nMin);
        puts_pad(maxima[i], nMax);
        puts(descs[i]); // this adds a newline
    }

    closedir(dirp);
}


void load(DIR* dirp, int num, const char* cfg_mgmt_path, char** names, char** values,
            char** minima, char** maxima, char** descs)
{
    const int fn_buf_len = 256;
    char fn[fn_buf_len];
    struct dirent *ep;
    int ret;
    // load all required files into memory
    int i = 0;
    while (ep=readdir(dirp)) {
        // ignore all files starting with a .
        if (ep->d_name[0] != '.')
            continue;
        // make sure the index does not overflow
        if (i >= num)
            break;
        // get memory and copy the name
        names[i] = malloc(strlen(ep->d_name)+1);
        strcpy(names[i], ep->d_name);
        // load value
        snprintf(fn, fn_buf_len, "%s/val/%s", cfg_mgmt_path, names[i]);
        values[i] = malloc(MAX_NUMERIC_LEN);
        ret = load_file(fn, values[i], MAX_NUMERIC_LEN);
        if (ret < 0) {
            fprintf(stderr, "%s: can't read file %s: %d\n", __func__, fn, ret);
            exit(-1);
        }
        // load minimum
        snprintf(fn, fn_buf_len, "%s/min/%s", cfg_mgmt_path, names[i]);
        minima[i] = malloc(MAX_NUMERIC_LEN);
        ret = load_file(fn, minima[i], MAX_NUMERIC_LEN);
        if (ret < 0) {
            fprintf(stderr, "%s: can't read file %s: %d\n", __func__, fn, ret);
            exit(-1);
        }
        // load minimum
        snprintf(fn, fn_buf_len, "%s/max/%s", cfg_mgmt_path, names[i]);
        maxima[i] = malloc(MAX_NUMERIC_LEN);
        ret = load_file(fn, maxima[i], MAX_NUMERIC_LEN);
        if (ret < 0) {
            fprintf(stderr, "%s: can't read file %s: %d\n", __func__, fn, ret);
            exit(-1);
        }
        // load description
        snprintf(fn, fn_buf_len, "%s/descs/%s", cfg_mgmt_path, names[i]);
        descs[i] = malloc(MAX_DESC_LEN);
        ret = load_file(fn, descs[i], MAX_DESC_LEN);
        if (ret < 0) {
            fprintf(stderr, "%s: can't read file %s: %d\n", __func__, fn, ret);
            exit(-1);
        }
        i++;
    }

}

// reads the (\0 terminated) content of file fn into buf, will not read more than n-1 chars
// returns number of bytes read (without \0) on success or neg error code
int load_file(const char* fn, char* buf, int n)
{
    int fd = open(fn,O_RDONLY);
    if(fd<0) {
        printf("%s: can't open file: %d\n", __func__, fd);
        return -errno;
    }
    int ret, bytes=0;
    n--; // make sure we have space for a trailing \0
    // read whole file or until the buffer is full
    while((ret=read(fd,buf+bytes,n-bytes))>0)
            bytes += ret;

    if (ret < 0)
        return ret;

    buf[bytes] = '\0';
    return bytes;
}


// print (stdout) str and pad with ' ' to a total of len chars
void puts_pad(const char* str, int len)
{
    fputs(str, stdout);
    for (int i=strlen(str); i<len; i++)
        fputc(' ', stdout);
}



// print help text
void help()
{
printf ("\
cfg_mgmt tools - clist\n\
 Lists all variables, current values, limits and descriptions\n\
 Options:\n\
  -d <path>   Specify path to cfg_mgmt folder exported by the kernel module\n\
              Default location: %s\n",DFLT_PATH);
}

