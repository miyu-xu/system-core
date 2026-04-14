
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/kdev_t.h>

#include <private/android_filesystem_config.h>
#include <private/fs_config.h>

#include <android-base/file.h>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

/* NOTES
**
** - see https://www.kernel.org/doc/Documentation/early-userspace/buffer-format.txt
**   for an explanation of this file format
** - dotfiles are ignored
** - directories named 'root' are ignored
*/

struct fs_config_entry {
    int uid, gid, mode;
};

static std::map<std::string, fs_config_entry> canned_config;
static const char* target_out_path = NULL;

#define TRAILER "TRAILER!!!"

static int total_size = 0;

static void fix_stat(const char *path, struct stat *s)
{
    uint64_t capabilities;
    if (!canned_config.empty()) {
        auto it = canned_config.find(path);
        if (it == canned_config.end()) it = canned_config.find("");

        if (it != canned_config.end()) {
            s->st_uid = it->second.uid;
            s->st_gid = it->second.gid;
            s->st_mode = it->second.mode | (s->st_mode & ~07777);
        }
    } else {
        // Use the compiled-in fs_config() function.
        unsigned st_mode = s->st_mode;
        int is_dir = S_ISDIR(s->st_mode) || strcmp(path, TRAILER) == 0;
        fs_config(path, is_dir, target_out_path, &s->st_uid, &s->st_gid, &st_mode, &capabilities);
        s->st_mode = (typeof(s->st_mode)) st_mode;
    }

    if (S_ISREG(s->st_mode) || S_ISDIR(s->st_mode) || S_ISLNK(s->st_mode)) {
        s->st_rdev = 0;
    }
}

static void _eject(struct stat *s, const char *out, int olen, char *data, unsigned datasize)
{
    // Nothing is special about this value, just picked something in the
    // approximate range that was being used already, and avoiding small
    // values which may be special.
    static unsigned next_inode = 300000;

    while(total_size & 3) {
        total_size++;
        putchar(0);
    }

    fix_stat(out, s);
//    fprintf(stderr, "_eject %s: mode=0%o\n", out, s->st_mode);

    printf("%06x%08x%08x%08x%08x%08x%08x"
           "%08x%08x%08x%08x%08x%08x%08x%s%c",
           0x070701,
           next_inode++,  //  s.st_ino,
           s->st_mode,
           0, // s.st_uid,
           0, // s.st_gid,
           1, // s.st_nlink,
           0, // s.st_mtime,
           datasize,
           0, // volmajor
           0, // volminor
           major(s->st_rdev),
           minor(s->st_rdev),
           olen + 1,
           0,
           out,
           0
           );

    total_size += 6 + 8*13 + olen + 1;

    if(strlen(out) != (unsigned int)olen) errx(1, "ACK!");

    while(total_size & 3) {
        total_size++;
        putchar(0);
    }

    if(datasize) {
        fwrite(data, datasize, 1, stdout);
        total_size += datasize;
    }
}

static void _eject_trailer()
{
    struct stat s;
    memset(&s, 0, sizeof(s));
    _eject(&s, TRAILER, 10, 0, 0);

    while(total_size & 0xff) {
        total_size++;
        putchar(0);
    }
}

static void _archive(char *in, char *out, int ilen, int olen);

static void _archive_dir(char *in, char *out, int ilen, int olen)
{
    std::unique_ptr<DIR, decltype(&closedir)> d(opendir(in), closedir);
    if (!d) err(1, "cannot open directory '%s'", in);

    std::vector<std::string> names;
    struct dirent *de;
    while((de = readdir(d.get())) != 0){
            /* xxx: feature? maybe some dotfiles are okay */
        if(de->d_name[0] == '.') continue;

            /* xxx: hack. use a real exclude list */
        if(!strcmp(de->d_name, "root")) continue;

        names.push_back(de->d_name);
    }

    std::sort(names.begin(), names.end());

    for (const auto& name : names) {
        in[ilen] = '/';
        memcpy(in + ilen + 1, name.c_str(), name.size() + 1);

        if(olen > 0) {
            out[olen] = '/';
            memcpy(out + olen + 1, name.c_str(), name.size() + 1);
            _archive(in, out, ilen + name.size() + 1, olen + name.size() + 1);
        } else {
            memcpy(out, name.c_str(), name.size() + 1);
            _archive(in, out, ilen + name.size() + 1, name.size());
        }

        in[ilen] = 0;
        out[olen] = 0;
    }
}

static void _archive(char *in, char *out, int ilen, int olen)
{
    struct stat s;
    if(lstat(in, &s)) err(1, "could not stat '%s'", in);

    if(S_ISREG(s.st_mode)){
        std::string content;
        if (!android::base::ReadFileToString(in, &content)) {
            err(1, "cannot read '%s'", in);
        }

        _eject(&s, out, olen, content.data(), content.size());
    } else if(S_ISDIR(s.st_mode)) {
        _eject(&s, out, olen, 0, 0);
        _archive_dir(in, out, ilen, olen);
    } else if(S_ISLNK(s.st_mode)) {
        char buf[1024];
        int size;
        size = readlink(in, buf, 1024);
        if(size < 0) err(1, "cannot read symlink '%s'", in);
        _eject(&s, out, olen, buf, size);
    } else if(S_ISBLK(s.st_mode) || S_ISCHR(s.st_mode) ||
              S_ISFIFO(s.st_mode) || S_ISSOCK(s.st_mode)) {
        _eject(&s, out, olen, NULL, 0);
    } else {
        errx(1, "Unknown '%s' (mode %d)?", in, s.st_mode);
    }
}

static void archive(const char* start, const char* prefix) {
    char in[8192];
    char out[8192];

    strcpy(in, start);
    strcpy(out, prefix);

    _archive_dir(in, out, strlen(in), strlen(out));
}

static void read_canned_config(char* filename)
{
    canned_config.clear();

    FILE* fp = fopen(filename, "r");
    if (fp == NULL) err(1, "failed to open canned file '%s'", filename);

    char* line = NULL;
    size_t allocated_len = 0;
    while (getline(&line, &allocated_len, fp) != -1) {
        if (!line[0]) break;

        char* name = NULL;
        char* uid_str = NULL;
        if (isspace(line[0])) {
            uid_str = strtok(line, " \n");
        } else {
            name = strtok(line, " \n");
            uid_str = strtok(NULL, " \n");
        }

        fs_config_entry cc = {};
        if (uid_str) cc.uid = atoi(uid_str);

        char* gid_str = strtok(NULL, " \n");
        if (gid_str) cc.gid = atoi(gid_str);

        char* mode_str = strtok(NULL, " \n");
        if (mode_str) cc.mode = strtol(mode_str, NULL, 8);

        canned_config.emplace(name ?: "", cc);
    }

    free(line);
    fclose(fp);
}

static void devnodes_desc_error(const char* filename, unsigned long line_num,
                              const char* msg)
{
    errx(1, "failed to read nodes desc file '%s' line %lu: %s", filename, line_num, msg);
}

static int append_devnodes_desc_dir(char* path, char* args)
{
    struct stat s;

    if (sscanf(args, "%o %d %d", &s.st_mode, &s.st_uid, &s.st_gid) != 3) return -1;

    s.st_mode |= S_IFDIR;

    _eject(&s, path, strlen(path), NULL, 0);

    return 0;
}

static int append_devnodes_desc_nod(char* path, char* args)
{
    int minor, major;
    struct stat s;
    char dev;

    if (sscanf(args, "%o %d %d %c %d %d", &s.st_mode, &s.st_uid, &s.st_gid,
               &dev, &major, &minor) != 6) return -1;

    s.st_rdev = MKDEV(major, minor);
    switch (dev) {
    case 'b':
        s.st_mode |= S_IFBLK;
        break;
    case 'c':
        s.st_mode |= S_IFCHR;
        break;
    default:
        return -1;
    }

    _eject(&s, path, strlen(path), NULL, 0);

    return 0;
}

static void append_devnodes_desc(const char* filename)
{
    FILE* fp = fopen(filename, "re");
    if (!fp) err(1, "failed to open nodes description file '%s'", filename);

    unsigned long line_num = 0;

    char* line = NULL;
    size_t allocated_len;
    while (getline(&line, &allocated_len, fp) != -1) {
        char *type, *path, *args;

        line_num++;

        if (*line == '#') continue;

        if (!(type = strtok(line, " \t"))) {
            devnodes_desc_error(filename, line_num, "a type is missing");
        }

        if (*type == '\n') continue;

        if (!(path = strtok(NULL, " \t"))) {
            devnodes_desc_error(filename, line_num, "a path is missing");
        }

        if (!(args = strtok(NULL, "\n"))) {
            devnodes_desc_error(filename, line_num, "args are missing");
        }

        if (!strcmp(type, "dir")) {
            if (append_devnodes_desc_dir(path, args)) {
                devnodes_desc_error(filename, line_num, "bad arguments for dir");
            }
        } else if (!strcmp(type, "nod")) {
            if (append_devnodes_desc_nod(path, args)) {
                devnodes_desc_error(filename, line_num, "bad arguments for nod");
            }
        } else {
            devnodes_desc_error(filename, line_num, "type unknown");
        }
    }

    free(line);
    fclose(fp);
}

static const struct option long_options[] = {
    { "dirname",    required_argument,  NULL,   'd' },
    { "file",       required_argument,  NULL,   'f' },
    { "help",       no_argument,        NULL,   'h' },
    { "nodes",      required_argument,  NULL,   'n' },
    { NULL,         0,                  NULL,   0   },
};

static void usage(void)
{
    fprintf(stderr,
            "Usage: mkbootfs [-n FILE] [-d DIR|-f FILE] DIR...\n"
            "\n"
            "\t-d, --dirname=DIR: fs-config directory\n"
            "\t-f, --file=FILE: Canned configuration file\n"
            "\t-h, --help: Print this help\n"
            "\t-n, --nodes=FILE: Dev nodes description file\n"
            "\n"
            "Dev nodes description:\n"
            "\t[dir|nod] [perms] [uid] [gid] [c|b] [major] [minor]\n"
            "\tExample:\n"
            "\t\t# My device nodes\n"
            "\t\tdir dev 0755 0 0\n"
            "\t\tnod dev/null 0600 0 0 c 1 3\n"
    );
}

int main(int argc, char *argv[])
{
    int opt, unused;

    while ((opt = getopt_long(argc, argv, "hd:f:n:", long_options, &unused)) != -1) {
        switch (opt) {
        case 'd':
            target_out_path = argv[optind - 1];
            break;
        case 'f':
            read_canned_config(argv[optind - 1]);
            break;
        case 'h':
            usage();
            return 0;
        case 'n':
            append_devnodes_desc(argv[optind - 1]);
            break;
        default:
            usage();
            errx(1, "Unknown option %s", argv[optind - 1]);
        }
    }

    int num_dirs = argc - optind;
    argv += optind;

    while (num_dirs-- > 0){
        char *x = strchr(*argv, '=');
        if (x != nullptr) {
            *x++ = '\0';
        }
        archive(*argv, x ?: "");

        argv++;
    }

    _eject_trailer();

    return 0;
}
