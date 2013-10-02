/* This is Fleece a simple stdin to jsonified UDP stream software

   Copyright (C) 2013  Aurelien Rougemont <beorn@gandi.net> <beorn@binaries.fr>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*
   fleece is heavily inspired of lumberjack
     https://github.com/jordansissel/lumberjack/tree/old/lumberjack-c
   By design lumberjacks can (and is) deadlocking apache. Because of this
   i have had to write something equivalent but with udp instead of 0mq
*/

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <jansson.h>
#include "str.h"

#define BUFFERSIZE 16384
#define FLEECE_VERSION "0.1"

extern char *strdup(const char *s);
extern char *strsep(char **stringp, const char *delim);
extern int gethostname(char *name, size_t namelen);

typedef enum {
  opt_help = 'h',
  opt_version = 'v',
  opt_field,
  opt_host,
  opt_port,
  opt_window_size,
} optlist_t;

struct option_doc {
  const char *name;
  int         has_arg;
  int         val;
  const char *documentation;
};

struct kv {
  char *key;
  size_t key_len;
  char *value;
  size_t value_len;
}; /* struct kv */

static struct option_doc options[] = {
  { "help", no_argument, opt_help, "show this help" },
  { "version", no_argument, opt_version, "show the version of fleece" },
  { "field", required_argument, opt_field, 
    "Add a custom key-value mapping to every line emitted" },
  { "host", required_argument, opt_host,
    "The hostname to send udp messages to" },
  { "port", required_argument, opt_port,
    "The port to connect on the udp server" },
  { "window_size", optional_argument, opt_window_size,
    "The window size" },
  { NULL, 0, 0, NULL },
};

void usage(const char *prog) {
  printf("Usage: %s [options]]\n", prog);

  for (int i = 0; options[i].name != NULL; i++) {
    printf("  --%s%s %.*s %s\n", options[i].name,
           options[i].has_arg ? " VALUE" : "",
           (int)(20 - strlen(options[i].name) - (options[i].has_arg ? 6 : 0)),
           "                                   ",
           options[i].documentation);
  }
} /* usage */

int main(int argc, char**argv)
{
    int sockfd,i,c;
    struct sockaddr_in servaddr;
    char sendline[1024];

    struct option *getopt_options = NULL;

    char hostname[200];

    struct kv *extra_fields = NULL;
    size_t extra_fields_len = 0;

    /* convert the 'option_doc' array into a 'struct option' array 
     * for use with getopt_long_only */
    for (i = 0; options[i].name != NULL; i++) {
      getopt_options = realloc(getopt_options, (i+1) * sizeof(struct option));
      getopt_options[i].name = options[i].name;
      getopt_options[i].has_arg = options[i].has_arg;
      getopt_options[i].flag = NULL;
      getopt_options[i].val = options[i].val;
    }

    /* Add one last item for the list terminator NULL */
    getopt_options = realloc(getopt_options, (i+1) * sizeof(struct option));
    getopt_options[i].name = NULL;
  
    char *tmp;
    char *host;
    unsigned short port;
    size_t window_size;
    window_size = (size_t)1024;

    while (i = -1, \
	c = getopt_long_only(argc, argv, "+hv", getopt_options, &i), c != -1) {
      switch (c) {
        case opt_version:
          printf("Fleece version %s\n",FLEECE_VERSION);
	  return 0;
        case opt_help:
          usage(argv[0]);
          return 0;
        case opt_host:
          host = strdup(optarg);
          break;
        case opt_port:
          port = (unsigned short)atoi(optarg);
          break;
        case opt_window_size:
          window_size = (size_t)atoi(optarg);
          break;
        case opt_field:
          tmp = strchr(optarg, '=');
          if (tmp == NULL) {
            printf("Invalid --field : expected 'foo=bar' form " \
                   "didn't find '=' in '%s'", optarg);
            usage(argv[0]);
            exit(1);
          }
          extra_fields_len += 1;
          extra_fields = realloc(extra_fields, \
		extra_fields_len * sizeof(struct kv));
          *tmp = '\0'; /* turn '=' into null terminator */
          tmp++; /* skip to first char of value */
          extra_fields[extra_fields_len - 1].key = strdup(optarg);
          extra_fields[extra_fields_len - 1].key_len = strlen(optarg);
          extra_fields[extra_fields_len - 1].value = strdup(tmp);
          extra_fields[extra_fields_len - 1].value_len = strlen(tmp);
          break;
        default:
	  printf("Not handled\n");
  
          usage(argv[0]);
          return 1;
      }
    }
    free(getopt_options);
  
    if (host == NULL) {
      printf("Missing --host flag\n");
      usage(argv[0]);
      return 1;
    }
  
    if (port == 0) {
      printf("Missing --port flag\n");
      usage(argv[0]);
      return 1;
    }
  
    argc -= optind;
    argv += optind;

    /*
     * prepare info to send stuff
     */

    gethostname(hostname, sizeof(hostname));

    sockfd=socket(AF_INET,SOCK_DGRAM,0);

    bzero(&servaddr,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr=inet_addr(host);
    servaddr.sin_port=htons(port);

    printf("fleece: sending jsonified stdin to %s:%i\n", host, port);

    while (fgets(sendline, window_size, stdin) != NULL)
    {
       if (strlen(sendline) == 0)
       {
           /* no data let's not burn cpu for nothing */
           sleep(1);
       } else {
           json_t *jsonevent;
           json_error_t jsonerror;
	   char *jsoneventstring;

           /* hey there's data let's process it */
	   jsonevent = json_loads(sendline, 0, &jsonerror);
           if (jsonevent== NULL)
	   {
		/* json not parsed ok then push jsonified version of msg */
	   	jsonevent = json_object();
		json_object_set(jsonevent, "message", json_string(sendline)); 
           }
           /* json parsed ok */
	   for (size_t i = 0; i < extra_fields_len; i++)
	   {
	   	json_object_set(jsonevent, extra_fields[i].key, \
	   		 json_string(extra_fields[i].value));
	   }
	
            /* add mandatory fields */
	   json_object_set(jsonevent, "file", json_string("-"));
	   json_object_set(jsonevent, "host", json_string(hostname)); 

	   /* copy modified json string to sendline */
	   jsoneventstring = json_dumps(jsonevent,JSON_COMPACT);
	   strcpy(sendline, jsoneventstring);

	   sendto(sockfd, sendline, strlen(sendline), 0, \
	   	(struct sockaddr *)&servaddr, sizeof(servaddr));
	   free(jsonevent);
      }
    } /* loop forever, reading from a file */
}
