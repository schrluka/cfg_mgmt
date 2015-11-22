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
#include <sys/select.h>
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

int load(struct dirent** name_list, int num, const char* cfg_mgmt_path, char** names, char** values,
            char** minima, char** maxima, char** descs);

int load_file(const char* fn, char* buf, int n);

int read_fd(int fd, char* buf, int n);

void help();



/******************************************************************************************************************************
*   I M P L E M E N T A T I O N
*/

int main (int argc, char **argv)
{
    char* cfg_mgmt_path = DFLT_PATH;  // location where debugfs is mounted
    //int index;
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
    int ret = show(cfg_mgmt_path);
    if (ret)
        printf("show returned %d\n", ret);
}


// simple filter which removes all files starting with . from the list of variables
int no_dot_filter(const struct dirent* de)
{
     return (de->d_name[0] != '.');
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

    struct dirent** name_list;
    char fn[fn_buf_len];

    // read all file names in the directory, remove all which start with a . and sort them alphabetically
    snprintf(fn, fn_buf_len, "%s/val", cfg_mgmt_path);
    num = scandir(fn, &name_list, &no_dot_filter, alphasort);
    if (num < 0) {
        printf("scandir error: %d\n", errno);
        return -errno;
    }
    if (num == 0) {
        printf("no variables found\n");
        return 0;
    }

    // get memory for the pointers
    names = malloc(num*sizeof(char*));
    values = malloc(num*sizeof(char*));
    minima = malloc(num*sizeof(char*));
    maxima = malloc(num*sizeof(char*));
    descs = malloc(num*sizeof(char*));
    // load all file contents
    ret = load(name_list, num, cfg_mgmt_path, names, values, minima, maxima, descs);
    if (ret) {
        printf("load returned %d\n", ret);
        return ret;
    }

    // free memory allocated by scandir
    //printf("freeing scandir memory\n");
    for (i=0; i<num; i++)
        free(name_list[i]);
    free(name_list);

	// find max length of the columns (for pretty formatting)
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
	}

	// add to all length to include an extra space for the longest name
	nName += 2;
	nVal += 2;
	nMin += 2;
	nMax += 2;
    // print the header
    puts_pad("Name", nName);
    puts_pad("Value", nVal);
    puts_pad("Minimum", nMin);
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

    return 0;
}


int load(struct dirent** name_list, int num, const char* cfg_mgmt_path, char** names, char** values,
            char** minima, char** maxima, char** descs)
{
    const int fn_buf_len = 256;
    char fn[fn_buf_len];
    int ret;
    int fd[4];

    // load all required files into memory
    int i;
    for (i=0; i<num; i++) {
        // get memory and copy the name
        //printf("processing entry '%s'\n", name_list[i]->d_name);
        names[i] = malloc(strlen(name_list[i]->d_name)+1);
        strcpy(names[i], name_list[i]->d_name);
        values[i] = malloc(MAX_NUMERIC_LEN);
        minima[i] = malloc(MAX_NUMERIC_LEN);
        maxima[i] = malloc(MAX_NUMERIC_LEN);
        descs[i] = malloc(MAX_DESC_LEN);

        // open files for value, min, max and desc
        snprintf(fn, fn_buf_len, "%s/val/%s", cfg_mgmt_path, names[i]);
        fd[0] = open(fn,O_RDONLY);
        if(fd[0]<0) {
            printf("%s: can't open file: %d\n", __func__, fd[0]);
            return -errno;
        }

        snprintf(fn, fn_buf_len, "%s/min/%s", cfg_mgmt_path, names[i]);
        fd[1] = open(fn,O_RDONLY);
        if(fd[1]<0) {
            printf("%s: can't open file: %d\n", __func__, fd[1]);
            return -errno;
        }

        snprintf(fn, fn_buf_len, "%s/max/%s", cfg_mgmt_path, names[i]);
        fd[2] = open(fn,O_RDONLY);
        if(fd[2]<0) {
            printf("%s: can't open file: %d\n", __func__, fd[2]);
            return -errno;
        }

        snprintf(fn, fn_buf_len, "%s/desc/%s", cfg_mgmt_path, names[i]);
        fd[3] = open(fn,O_RDONLY);
        if(fd[3]<0) {
            printf("%s: can't open file: %d\n", __func__, fd[3]);
            return -errno;
        }

        // read the files
        ret = read_fd(fd[0], values[i], MAX_NUMERIC_LEN);
        if (ret < 0) {
            fprintf(stderr, "%s: can't read val file %d\n", __func__, ret);
            exit(-1);
        }
        ret = read_fd(fd[1], minima[i], MAX_NUMERIC_LEN);
        if (ret < 0) {
            fprintf(stderr, "%s: can't read min file: %d\n", __func__, ret);
            exit(-1);
        }
        ret = read_fd(fd[2], maxima[i], MAX_NUMERIC_LEN);
        if (ret < 0) {
            fprintf(stderr, "%s: can't read max file: %d\n", __func__, ret);
            exit(-1);
        }
        ret = read_fd(fd[3], descs[i], MAX_DESC_LEN);
        if (ret < 0) {
            fprintf(stderr, "%s: can't read desc file: %d\n", __func__, ret);
            exit(-1);
        }

        // close the files
        for (int j=0; j<4; j++)
            close(fd[j]);

    }
    return 0;
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

    // remove trailing \n
    if (buf[bytes-1] == '\n')
        buf[bytes-1] = '\0';

    buf[bytes] = '\0';

    close(fd);
    return bytes;
}

// read file descriptor fd into buf, max n bytes
// returns number of bytes read (without \0) on success or neg error code
int read_fd(int fd, char* buf, int n)
{
    int ret, bytes=0;
    n--; // make sure we have space for a trailing \0
    // read whole file or until the buffer is full
    while((ret=read(fd,buf+bytes,n-bytes))>0)
        bytes += ret;

    //printf("read %d bytes from fd %d\n", bytes, fd);

    if (ret < 0)
        return ret;

    // remove trailing \n
    if (buf[bytes-1] == '\n')
        buf[bytes-1] = '\0';

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

