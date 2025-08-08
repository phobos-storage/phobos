/*
 *  All rights reserved (c) 2014-2025 CEA/DAM.
 *
 *  This file is part of Phobos.
 *
 *  Phobos is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  Phobos is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with Phobos. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \brief  Simple script to send artificial requests to the LRS
 * and check the correct behavior of the I/O scheduler.
 */

#include "pho_test.h"

static void split(char sep, char *str, int *count, char ***list)
{
    char sepstr[] = { sep, '\0' };
    int capacity = 10;
    char *token;
    char *rest;

    *list = NULL;
    *count = 0;

    *list = xmalloc(sizeof(char **) * capacity);
    for (token = strtok_r(str, sepstr, &rest);
         token != NULL;
         token = strtok_r(NULL, sepstr, &rest)) {

        (*list)[*count] = xstrdup(token);
        (*count)++;
        if (*count >= capacity) {
            capacity *= 2;
            *list = xrealloc(*list, capacity * sizeof(*list));
        }
    }
}

static char *join(char sep, char **list, size_t size)
{
    const char separator[] = { sep, '\0' };
    size_t len = 0;
    char *iter;
    char *str;
    size_t i;

    for (i = 0; i < size; i++)
        len += strlen(list[i]) + 1;

    str = xmalloc(len + 1);
    iter = str;

    for (i = 0; i < size; i++) {
        strcpy(iter, list[i]);
        iter += strlen(list[i]);
        strcpy(iter, separator);
        iter += 1;
    }

    return str;
}

struct signal {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
};

static void signal_send(struct signal *signal)
{
    MUTEX_LOCK(&signal->mutex);
    pthread_cond_signal(&signal->cond);
    MUTEX_UNLOCK(&signal->mutex);
}

#define signal_wait_if(signal, condition)                         \
    do {                                                          \
        MUTEX_LOCK(&(signal)->mutex);                             \
        if (condition)                                            \
            pthread_cond_wait(&(signal)->cond, &(signal)->mutex); \
        MUTEX_UNLOCK(&(signal)->mutex);                           \
    } while(0)

static void signal_init(struct signal *signal)
{
    pthread_mutex_init(&signal->mutex, NULL);
    pthread_cond_init(&signal->cond, NULL);
}

struct client_context {
    struct tsqueue responses;
    struct tsqueue requests;
    int last_id;
    bool exit_on_error;
    FILE *source;
    bool stopping;
    pthread_t comm_thread;
    GHashTable *responses_by_id;
    struct signal new_request;
    struct signal new_response;
    size_t n_reads_inflight;
    size_t n_releases_inflight;
    size_t n_writes_inflight;
    struct dss_handle dss;
};

static void *comm_thread(void *data);

static int start_comm_thread(struct client_context *ctxt)
{
    signal_init(&ctxt->new_request);
    signal_init(&ctxt->new_response);
    return pthread_create(&ctxt->comm_thread, NULL, comm_thread, ctxt);
}

static void stop_comm_thread(struct client_context *ctxt)
{
    ctxt->stopping = true;
    signal_send(&ctxt->new_request);
    pthread_join(ctxt->comm_thread, NULL);
}

enum pho_client_command_result {
    PCCR_SUCCESS,
    PCCR_STOP,
    PCCR_HELP,
    PCCR_EOF,
};

typedef int (*pho_command_func_t)(struct client_context *ctxt,
                                  int argc, char **argv);

struct pho_command {
    const char *name;
    const char *help;
    pho_command_func_t run;
};

static int pc_batch(struct client_context *ctxt, int argc, char **argv);
static int pc_help(struct client_context *ctxt, int argc, char **argv);
static int pc_inflight(struct client_context *ctxt, int argc, char **argv);
static int pc_quit(struct client_context *ctxt, int argc, char **argv);
static int pc_read(struct client_context *ctxt, int argc, char **argv);
static int pc_recv(struct client_context *ctxt, int argc, char **argv);
static int pc_release(struct client_context *ctxt, int argc, char **argv);
static int pc_write(struct client_context *ctxt, int argc, char **argv);

enum pho_client_command {
    PCC_BATCH,
    PCC_HELP,
    PCC_INFLIGHT,
    PCC_QUIT,
    PCC_READ,
    PCC_RECV,
    PCC_RELEASE,
    PCC_WRITE,
    PCC_INVALID,
};

static enum pho_client_command str2pcc(const char *cmd)
{
    if (!strcmp(cmd, "batch"))
        return PCC_BATCH;
    else if (!strcmp(cmd, "help"))
        return PCC_HELP;
    else if (!strcmp(cmd, "inflight"))
        return PCC_INFLIGHT;
    else if (!strcmp(cmd, "quit"))
        return PCC_QUIT;
    if (!strcmp(cmd, "read"))
        return PCC_READ;
    else if (!strcmp(cmd, "recv"))
        return PCC_RECV;
    else if (!strcmp(cmd, "release"))
        return PCC_RELEASE;
    else if (!strcmp(cmd, "write"))
        return PCC_WRITE;
    else
        return PCC_INVALID;
}

static struct pho_command commands[] = {
    [PCC_BATCH] = {
        .name = "batch",
        .run = pc_batch,
        .help = "batch <n> subcommand ARGS...",
    },
    [PCC_HELP] = {
        .name = "help",
        .run  = pc_help,
        .help = "help [cmd]",
    },
    [PCC_INFLIGHT] = {
        .name = "inflight",
        .run  = pc_inflight,
        .help = "inflight [-r n] [-w n] [-R n]",
    },
    [PCC_QUIT] = {
        .name = "quit",
        .run  = pc_quit,
        .help = "quit",
    },
    [PCC_READ] = {
        .name = "read",
        .run  = pc_read,
        .help = "read n_required MEDIUM...",
    },
    [PCC_RECV] = {
        .name = "recv",
        .run  = pc_recv,
        .help = "recv id expected_type [MEDIA]",
    },
    [PCC_RELEASE] = {
        .name = "release",
        .run  = pc_release,
        .help = "release [-n count] [id]",
    },
    [PCC_WRITE] = {
        .name = "write",
        .run  = pc_write,
        .help = "write \n"
                "      [-t|--tags tag1,tag2]\n"
                "      [-g|--grouping grouping]\n"
                "      [-f|--family family]\n"
                "      [-l|--library library]\n"
                "      n_media [size]",
    },
};

static int pc_run(struct client_context *ctxt, int argc, char **argv);

static int pc_batch(struct client_context *ctxt, int argc, char **argv)
{
    int64_t count;
    int64_t i;

    if (argc <= 2)
        LOG_RETURN(-EINVAL, "missing arguments");

    count = str2int64(argv[1]);
    if (count <= 0)
        LOG_RETURN(-EINVAL, "invalid count '%s', expected integer > 0",
                   argv[1]);

    for (i = 0; i < count; i++) {
        int rc;

        rc = pc_run(ctxt, argc - 2, argv + 2);
        if (rc)
            return rc;
    }

    return 0;
}

static int default_library(enum rsc_family family, const char **library)
{
    char *key;
    int rc;

    if (asprintf(&key, "default_%s_library", rsc_family2str(family)) == -1)
        return -ENOMEM;

    rc = pho_cfg_get_val("store", key, library);
    free(key);
    return rc;
}

static int get_library_and_family(struct dss_handle *dss,
                                  char **media, size_t n_media,
                                  char **library,
                                  enum rsc_family *family)
{
    size_t i;
    int rc;

    *family = PHO_RSC_INVAL;
    *library = NULL;

    for (i = 0; i < n_media; i++) {
        struct media_info *res;
        struct dss_filter filter;
        int n_res;

        rc = dss_filter_build(&filter, "{\"DSS::MDA::id\": \"%s\"}", media[i]);
        if (rc)
            goto free_library;

        rc = dss_media_get(dss, &filter, &res, &n_res, NULL);
        dss_filter_free(&filter);
        if (rc)
            goto free_library;

        if (n_res > 1) {
            dss_res_free(res, n_res);
            LOG_GOTO(free_library, rc = -EINVAL,
                     "%d media found with id '%s'. "
                     "This program assumes that there is no duplicate",
                     n_res, media[i]);
        }

        if (!*library) {
            *library = xstrdup(res->rsc.id.library);
            *family = res->rsc.id.family;
        } else {
            if (strcmp(*library, res->rsc.id.library) ||
                *family != res->rsc.id.family)
                LOG_GOTO(free_library, rc = -EINVAL,
                         "this program only support reads for the"
                         " same family and library");
        }
    }

    return 0;

free_library:
    free(*library);
    *library = NULL;

    return rc;
}

static int pc_read(struct client_context *ctxt,
                    int argc, char **argv)
{
    enum rsc_family family;
    int64_t n_required;
    pho_req_t *read;
    size_t n_media;
    char *library;
    char **media;
    int rc;

    if (argc < 3) {
        pho_error(0, "invalid arguments");
        return PCCR_HELP;
    }

    /* ignore first 2 arguments "read n_required MEDIA..." */
    n_media = argc - 2;
    media = argv + 2;
    n_required = str2int64(argv[1]);
    if (n_required < 1) {
        pho_error(0, "n_required '%s' invalid. Expect a number >= 1",
                  argv[1]);
        return PCCR_HELP;
    }

    rc = get_library_and_family(&ctxt->dss, media, n_media, &library, &family);
    if (rc)
        return rc;

    read = make_read_request(ctxt->last_id++, n_media, n_required,
                             PHO_RSC_DIR, (const char **)media,
                             library);
    tsqueue_push(&ctxt->requests, read);
    signal_send(&ctxt->new_request);
    free(library);

    return 0;
}

static struct string_array parse_tags(char *tags)
{
    struct string_array res;
    int count;

    split(',', tags, &count, &res.strings);
    res.count = count;

    return res;
}

/*  write [-t|--tags tag1,tag2]
 *        [-g|--grouping grouping]
 *        [-f|--family]
 *        [-l|--library]
 *        n_media [size] */
static int pc_write(struct client_context *ctxt,
                     int argc, char **argv)
{
    struct option options[] = {
        {"grouping",    no_argument,       0,  'g'},
        {"family",      no_argument,       0,  'f'},
        {"library",     no_argument,       0,  'l'},
        {"grouping",    no_argument,       0,  'g'},
        {"tags",        no_argument,       0,  't'},
        {0,             0,                 0,  0}
    };
    enum rsc_family family = PHO_RSC_DIR;
    struct string_array tags = {0};
    const char *grouping = NULL;
    const char *library = NULL;
    pho_req_t *write;
    int64_t n_media;
    int64_t size;
    char c;
    int rc;

    while ((c = getopt_long(argc, argv, "t:g:", options, NULL)) != -1) {
        switch (c) {
        default:
            printf("Invalid arguments\n");
            return PCCR_HELP;
        case 'f':
            family = str2rsc_family(optarg);
            if (family == PHO_RSC_INVAL)
                LOG_RETURN(-EINVAL, "invalid family '%s'", optarg);
            break;
        case 'l':
            library = xstrdup(optarg);
            break;
        case 'g':
            grouping = xstrdup(optarg);
            break;
        case 't':
            tags = parse_tags(optarg);
            break;
        }
    }

    if (!library) {
        rc = default_library(family, &library);
        if (rc)
            LOG_RETURN(rc, "failed to retrieve default library");

        library = xstrdup_safe(library);
    }

    if (optind == argc) {
        printf("Missing 'n_media' argument\n");
        return -EINVAL;
    }

    n_media = str2int64(argv[optind]);
    if (n_media == INT64_MIN || n_media == 0)
        LOG_RETURN(-EINVAL, "'%s' is not a valid number of media",
                   argv[optind]);

    if (optind == argc + 1) {
        size = str2int64(argv[optind + 1]);
        if (size == INT64_MIN || size == 0)
            LOG_RETURN(-EINVAL, "'%s' is not a valid size",
                       argv[optind]);
    } else {
        size = 4096;
    }

    write = make_write_request(ctxt->last_id++, n_media, size,
                               family, grouping, &tags, library);
    tsqueue_push(&ctxt->requests, write);
    signal_send(&ctxt->new_request);

    return 0;
}

static char *replace(const char *str, char from, char to)
{
    char *dup = xstrdup(str);
    size_t i = 0;

    for (i = 0; i < strlen(dup); i++)
        if (dup[i] == from)
            dup[i] = to;

    return dup;
}

static int pc_recv(struct client_context *ctxt,
                    int argc, char **argv)
{
    char *response_type = NULL;
    pho_resp_t *resp = NULL;
    const char *type;
    int type_idx;
    int id_idx;
    int64_t id;
    int rc;

    if (argc < 2)
        LOG_RETURN(-EINVAL, "invalid number of arguments");

    if (argc == 2) {
        type_idx = 1;
        id_idx = -1;
    } else if (argc == 3) {
        type_idx = 2;
        id_idx = 1;
    } else {
        LOG_RETURN(-EINVAL, "invalid number of arguments");
    }

    type = argv[type_idx];
    if (id_idx != -1) {
        id = str2int64(argv[id_idx]);
        if (id == INT64_MIN || id == 0)
            LOG_RETURN(-EINVAL, "'%s' is not a valid id %ld",
                       argv[optind], id);
    }

    if (tsqueue_get_length(&ctxt->responses) == 0) {
        /* Wake up comm thread in case it didn't read the request on the
         * last iteration.
         */
        signal_send(&ctxt->new_request);
        signal_wait_if(&ctxt->new_response,
                       tsqueue_get_length(&ctxt->responses) == 0);
    }

    resp = tsqueue_pop(&ctxt->responses);
    if (!resp)
        pho_exit(EFAULT, "Response queue is empty");

    if (id_idx != -1 && resp->req_id != id)
        LOG_GOTO(free_resp, rc = -EINVAL,
                 "unexpected request ID. Expected '%ld', got '%d'",
                   id, resp->req_id);

    // replace ' ' by '_' to avoid having to handle spaces in command
    // parameters.
    response_type = replace(pho_srl_response_kind_str(resp),
                            ' ', '_');
    if (strcmp(response_type, type))
        LOG_GOTO(free_resp, rc = -EINVAL,
                 "unexpected response type. Expected '%s', got '%s'",
                   type, response_type);

    g_hash_table_insert(ctxt->responses_by_id, &resp->req_id, resp);
    free(response_type);

    return 0;

free_resp:
    free(response_type);
    if (resp)
        pho_srl_response_free(resp, true);

    return rc;
}

static int release_n(struct client_context *ctxt, size_t n, bool async)
{
    GHashTableIter iter;
    gpointer value;
    gpointer key;

    g_hash_table_iter_init(&iter, ctxt->responses_by_id);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        pho_resp_t *resp = value;
        pho_req_t *req;

        if (n == 0)
            break;

        if (!pho_response_is_read(resp) &&
            !pho_response_is_write(resp))
            continue;

        req = make_release_request(resp, ctxt->last_id++, async);
        tsqueue_push(&ctxt->requests, req);
        g_hash_table_iter_remove(&iter);
        pho_srl_response_free(resp, true);
        n--;
    }

    if (n != 0)
        return -ERANGE;

    signal_send(&ctxt->new_request);
    return 0;
}

static int release_all_async(struct client_context *ctxt)
{
    release_n(ctxt, g_hash_table_size(ctxt->responses_by_id), true);
    return 0;
}

static int release_all(struct client_context *ctxt)
{
    release_n(ctxt, g_hash_table_size(ctxt->responses_by_id), false);
    return 0;
}

static int pc_release(struct client_context *ctxt, int argc, char **argv)
{
    struct option options[] = {
        {"num",    no_argument,   0,  'n'},
        {0,        0,             0,  0}
    };
    int64_t n_release = 0;
    pho_resp_t *resp;
    uint32_t req_id;
    int64_t value;
    char c;

    if (argc == 1)
        return release_all(ctxt);

    while ((c = getopt_long(argc, argv, "n:", options, NULL)) != -1) {
        switch (c) {
        default:
            printf("Invalid arguments\n");
            return PCCR_HELP;
        case 'n':
            n_release = str2int64(optarg);
            if (n_release < 0)
                LOG_RETURN(-EINVAL,
                           "invalid argument to '-w'. Expected positive integer, got '%s'",
                           optarg);
            break;
        }
    }

    if (n_release)
        return release_n(ctxt, n_release, false);

    value = str2int64(argv[1]);
    if (value < 0)
        LOG_RETURN(-EINVAL,
                   "'%s' is not a valid request id (must be a positive integer)",
                   argv[1]);

    req_id = value;
    resp = g_hash_table_lookup(ctxt->responses_by_id, &req_id);
    if (!resp)
        LOG_RETURN(-ENOENT, "response for request %s not received",
                   argv[1]);

    tsqueue_push(&ctxt->requests,
                 make_release_request(resp, ctxt->last_id++, false));
    signal_send(&ctxt->new_request);

    g_hash_table_remove(ctxt->responses_by_id, &req_id);
    pho_srl_response_free(resp, true);

    return 0;
}

static int pc_inflight(struct client_context *ctxt, int argc, char **argv)
{
    struct option options[] = {
        {"reads",       no_argument,       0,  'r'},
        {"writes",      no_argument,       0,  'w'},
        {"releases",    no_argument,       0,  'R'},
        {0,             0,                 0,  0}
    };
    int64_t n_releases = 0;
    int64_t n_writes = 0;
    int64_t n_reads = 0;
    char c;

    while ((c = getopt_long(argc, argv, "r:w:R:", options, NULL)) != -1) {
        switch (c) {
        default:
            printf("Invalid arguments\n");
        case 'R':
            n_releases = str2int64(optarg);
            if (n_releases < 0)
                LOG_RETURN(-EINVAL,
                           "invalid argument to '-R'. Expected positive integer, got '%s'",
                           optarg);
            break;
        case 'r':
            n_reads = str2int64(optarg);
            if (n_reads < 0)
                LOG_RETURN(-EINVAL,
                           "invalid argument to '-r'. Expected positive integer, got '%s'",
                           optarg);
            break;
        case 'w':
            n_writes = str2int64(optarg);
            if (n_writes < 0)
                LOG_RETURN(-EINVAL,
                           "invalid argument to '-w'. Expected positive integer, got '%s'",
                           optarg);
            break;
        }
    }

    /* This is not protected by a lock even though it is increased in
     * a different thread. This is because in practice, checking the
     * number of inflight messages must be done when this number is
     * stable. Otherwise the tests would be unreliable. The user of
     * this command must ensure that recv is called at least once
     * before calling inflight and that no new requests have been sent.
     * This will ensure that the thread is idle, that all the pending
     * requests have been pushed to phobosd and that the responses have
     * either been received or the thread is blocked in the receive call.
     */
    if (ctxt->n_reads_inflight != n_reads)
        LOG_RETURN(-EINVAL,
                   "invalid number of read requests in flight. Expected '%ld', got '%lu",
                   n_reads, ctxt->n_reads_inflight);

    if (ctxt->n_writes_inflight != n_writes)
        LOG_RETURN(-EINVAL,
                   "invalid number of write requests in flight. Expected '%ld', got '%lu",
                   n_writes, ctxt->n_writes_inflight);

    if (ctxt->n_releases_inflight != n_releases)
        LOG_RETURN(-EINVAL,
                   "invalid number of release requests in flight. Expected '%ld', got '%lu",
                   n_writes, ctxt->n_releases_inflight);

    return 0;
}

static int pc_quit(struct client_context *ctxt,
                    int argc, char **argv)
{
    return PCCR_STOP;
}

static int pc_help(struct client_context *ctxt,
                    int argc, char **argv)
{
    if (argc == 1) {
        size_t i;

        printf("Commands:\n");
        for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
            printf("- %s\n", commands[i].name);
    } else if (argc == 2) {
        enum pho_client_command cmd;

        cmd = str2pcc(argv[1]);
        if (cmd == PCC_INVALID) {
            printf("Invalid command '%s'\n", argv[1]);
            return -EINVAL;
        }
        printf("Usage %s:\n", commands[cmd].name);
        printf("%s\n", commands[cmd].help);
    }
    return 0;
}

static int pc_run(struct client_context *ctxt, int argc, char **argv)
{
    enum pho_client_command action;
    char *cmd;
    int rc;

    if (argc == 0)
        return 0;

    cmd = join(' ', argv, argc);
    pho_verb("running command: %s", cmd);
    free(cmd);

    action = str2pcc(argv[0]);
    if (action == PCC_INVALID) {
        LOG_RETURN(-EINVAL, "invalid command '%s'\n", argv[0]);
    }

    optind = 0;
    rc = commands[action].run(ctxt, argc, argv);
    if (rc == PCCR_HELP)
        printf("%s\n", commands[action].help);

    return rc;
}

static void pho_test_log_cb(const struct pho_logrec *rec)
{
    char *error_buffer = NULL;

    if (rec->plr_err != 0) {
        int rc;

        rc = asprintf(&error_buffer, ": %s (%d)",
                      strerror(rec->plr_err), rec->plr_err);
        if (rc < 0) {
            error_buffer = NULL;
        }
    }

    fprintf(stderr, "%s%s%s\n",
            error_buffer ? "error: " : "",
            rstrip(rec->plr_msg),
            error_buffer ? : "");

    free(error_buffer);
}

static void usage(int rc)
{
    printf(
        "Usage: %s [--stdin] [-v|--verbose] [-q|--quiet] [-c|--config path] [cmd args...]\n"
        "\n"
        "    --stdin        read commands from the standard input\n"
        "    -q|--quiet     be less verbose (can be repeated)\n"
        "    -v|--verbose   be more verbose (can be repeated)\n"
        "    -c|--config    path to phobos.conf\n"
        "\n"
        "To display the available commands:\n"
        "    %s help\n"
        ,
        program_invocation_short_name,
        program_invocation_short_name
    );
    exit(rc);
}

#define EXIT_ON_ERROR_OPT STDIN_OPT-1
#define STDIN_OPT CHAR_MAX

static int parse_args(int argc, char **argv, struct client_context *ctxt)
{
    struct option options[] = {
        {"help",          no_argument,       0,  'h'},
        {"verbose",       no_argument,       0,  'v'},
        {"config",        no_argument,       0,  'c'},
        {"quiet",         no_argument,       0,  'q'},
        {"exit-on-error", no_argument,       0,  EXIT_ON_ERROR_OPT},
        {"stdin",         no_argument,       0,  STDIN_OPT},
        {0,               0,                 0,  0}
    };
    int i = 1; /* always skip argv[0] */
    int verbose = PHO_LOG_INFO;
    const char *config = NULL;
    bool use_stdin = false;
    char c;
    int rc;

    ctxt->source = NULL;
    ctxt->exit_on_error = false;

    /* less verbose logs */
    pho_log_callback_set(pho_test_log_cb);

    while (i < argc && str2pcc(argv[i]) == PCC_INVALID) {
        i++;
    }

    while ((c = getopt_long(i, argv, "hvqc:", options, NULL)) != -1) {
        switch (c) {
        case 'c':
            config = xstrdup(optarg);
            break;
        case 'v':
            if (verbose < PHO_LOG_DEBUG)
                verbose++;
            break;
        case 'q':
            if (verbose > PHO_LOG_DISABLED)
                verbose--;
            break;
        case STDIN_OPT:
            use_stdin = true;
            break;
        case EXIT_ON_ERROR_OPT:
            ctxt->exit_on_error = true;
            break;
        case 'h':
            usage(EXIT_SUCCESS);
        default:
            usage(EXIT_FAILURE);
        }
    }

    rc = pho_cfg_init_local(config);
    if (rc && rc != -EALREADY)
        return rc;

    if (use_stdin && i != argc)
        LOG_RETURN(-EINVAL, "'--stdin' specified with a '%s' command", argv[i]);

    if (use_stdin)
        ctxt->source = stdin;

    pho_log_level_set(verbose);

    return i;
}

static char *read_line(FILE *source)
{
    static int lineno = -1;
    char buf[512];
    char *newline;
    char *s;

    s = fgets(buf, sizeof(buf), source);
    if (s == NULL)
        return NULL;

    newline = strchr(buf, '\n');
    if (!newline)
        pho_exit(EINVAL, "line %d too long", lineno);

    lineno++;
    *newline = '\0';
    return xstrdup(buf);
}

static int next_command(FILE *source, char **line, int *argc, char ***argv)
{
    *line = read_line(source);
    if (!*line) {
        *argc = 0;
        *argv = NULL;
        return PCCR_EOF;
    }

    if (!strcmp(*line, "") || (*line)[0] == '#') {
        *argc = 0;
        *argv = NULL;
        return 0;
    }

    split(' ', *line, argc, argv);

    return 0;
}

static int interpret_file(struct client_context *ctxt)
{
    size_t lineno = 1;
    char **argv;
    char *line;
    int argc;
    int rc;

    while ((rc = next_command(ctxt->source, &line, &argc, &argv)) != PCCR_EOF) {
        if (argc == 0)
            continue;

        rc = pc_run(ctxt, argc, argv);
        free(line);
        if (rc == PCCR_STOP)
            break;

        if (rc != PCCR_SUCCESS && ctxt->exit_on_error) {
            pho_error(rc, "error at line '%ld'", lineno);
            return rc;
        }

        lineno++;
    }

    return PCCR_SUCCESS;
}

static void handle_sigterm(int signum)
{
}

static void setup_signal(void)
{
    struct sigaction sig;

    sig.sa_handler = handle_sigterm;
    sig.sa_flags = 0;
    sigemptyset(&sig.sa_mask);
    sigaction(SIGTERM, &sig, NULL);
    sigaction(SIGINT, &sig, NULL);
}

static int start_repl(struct client_context *ctxt)
{
    setup_signal();

    while (true) {
        char **argv;
        char *line;
        int argc;
        int rc;

        printf("> ");

        rc = next_command(stdin, &line, &argc, &argv);
        if (argc == 0) {
            printf("\n");
            continue;
        }

        rc = pc_run(ctxt, argc, argv);
        free(line);
        if (rc == PCCR_STOP)
            break;

        if (rc != PCCR_SUCCESS && ctxt->exit_on_error)
            return rc;
    }

    return PCCR_SUCCESS;
}

static int set_nonblock(struct pho_comm_info *comm)
{
    int flags;

    flags = fcntl(comm->socket_fd, F_GETFL, 0);
    if (flags == -1)
        LOG_RETURN(-errno, "failed to get socket flags");

    if (fcntl(comm->socket_fd, F_SETFL, flags | O_NONBLOCK) == -1)
        LOG_RETURN(-errno, "failed to add O_NONBLOCK flag to socket");

    return 0;
}

static void *comm_thread(void *data)
{
    struct client_context *ctxt = data;
    struct pho_comm_info *comm;
    size_t inflight = 0;

    comm = phobosd_connect();
    if (!comm)
        return NULL;

    set_nonblock(comm);

    while (!ctxt->stopping) {
        pho_resp_t **resps;
        pho_req_t *req;
        int n_resps;
        int i;

        while ((req = tsqueue_pop(&ctxt->requests)) != NULL) {
            send_request(comm, req);
            if (pho_request_is_write(req))
                ctxt->n_writes_inflight++;
            else if (pho_request_is_read(req))
                ctxt->n_reads_inflight++;
            else if (pho_request_is_release_write(req))
                ctxt->n_releases_inflight++;

            if (!pho_request_is_release_read(req))
                /* do not increase for read release since we won't receive
                 * an answer.
                 */
                inflight++;
            pho_srl_request_free(req, false);
        }

        if (inflight == 0) {
            signal_wait_if(&ctxt->new_request, !ctxt->stopping);
            if (ctxt->stopping && tsqueue_get_length(&ctxt->requests) == 0)
                /* make sure to flush releases before stopping */
                break;

            continue;
        }

        /* Since this is blocking, we could be blocked
         * in a recv when stopping is set.
         */
        recv_responses(comm, &resps, &n_resps);
        if (n_resps == 0)
            continue;

        assert(inflight >= n_resps);
        inflight -= n_resps;
        for (i = 0; i < n_resps; i++) {
            if (pho_response_is_write(resps[i]))
                ctxt->n_writes_inflight--;
            else if (pho_response_is_read(resps[i]))
                ctxt->n_reads_inflight--;
            else if (pho_response_is_release_write(resps[i]))
                ctxt->n_releases_inflight--;
            tsqueue_push(&ctxt->responses, resps[i]);
        }

        signal_send(&ctxt->new_response);
    }

    phobosd_disconnect(comm);

    return NULL;
}

static guint glib_uint32_hash(gconstpointer _v)
{
    const uint32_t *v = _v;
    int64_t value = (int64_t)*v;

    return g_int64_hash(&value);
}

static gboolean glib_uint32_equal(gconstpointer _lhs,
                                  gconstpointer _rhs)
{
    const uint32_t *lhs = _lhs;
    const uint32_t *rhs = _rhs;

    return *lhs == *rhs;
}

int main(int argc, char **argv)
{
    struct client_context ctxt = {0};
    int skip;
    int rc;

    assert(PCCR_SUCCESS == 0);

    /* less verbose output from error(3) */
    program_invocation_name = program_invocation_short_name;

    phobos_init();
    atexit(phobos_fini);

    skip = parse_args(argc, argv, &ctxt);
    if (skip < 0)
        return EXIT_FAILURE;

    rc = dss_init(&ctxt.dss);
    if (rc)
        return EXIT_FAILURE;

    tsqueue_init(&ctxt.requests);
    tsqueue_init(&ctxt.responses);
    /* start IDs at 1 */
    ctxt.last_id = 1;
    ctxt.responses_by_id = g_hash_table_new(glib_uint32_hash,
                                            glib_uint32_equal);
    start_comm_thread(&ctxt);

    if (argc == skip) {
        if (ctxt.source)
            rc = interpret_file(&ctxt);
        else
            rc = start_repl(&ctxt);
    } else {
        rc = pc_run(&ctxt, argc - skip, argv + skip);
    }
    release_all_async(&ctxt);
    signal_send(&ctxt.new_request);

    stop_comm_thread(&ctxt);
    g_hash_table_destroy(ctxt.responses_by_id);
    dss_fini(&ctxt.dss);

    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
