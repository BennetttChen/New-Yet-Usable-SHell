// nyush.c - New Yet Usable SHell (nyush)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_TOK   2048
#define MAX_ARGS  256
#define MAX_JOBS  100

#define ERR_INVALID_CMD   "Error: invalid command\n"
#define ERR_INVALID_PROG  "Error: invalid program\n"
#define ERR_INVALID_FILE  "Error: invalid file\n"
#define ERR_INVALID_DIR   "Error: invalid directory\n"
#define ERR_INVALID_JOB   "Error: invalid job\n"
#define ERR_SUSP_JOBS     "Error: there are suspended jobs\n"

typedef struct Cmd {
    char *argv[MAX_ARGS];
    int   argc;
    char *infile;
    char *outfile;
    int   append;         // 0:>, 1:>>
    struct Cmd *next;     // pipeline next
} Cmd;

typedef struct Job {
    pid_t pid;
    char *cmdline;
} Job;

/* ===== Job list that preserves chronological order ===== */
static Job joblist[MAX_JOBS];
static int  njobs = 0;

static int jobs_count(void) { return njobs; }

static void jobs_print(void) {
    for (int i = 0; i < njobs; ++i) {
        printf("[%d] %s\n", i + 1, joblist[i].cmdline);
    }
}

static void jobs_push(pid_t pid, const char *cmdline) {
    if (njobs >= MAX_JOBS) return;
    joblist[njobs].pid = pid;
    joblist[njobs].cmdline = strdup(cmdline);
    njobs++;
}

static int jobs_index_from_print_index(int idx1) {
    int pos = idx1 - 1;
    if (pos < 0 || pos >= njobs) return -1;
    return pos;
}

static void jobs_remove_at(int pos) {
    if (pos < 0 || pos >= njobs) return;
    free(joblist[pos].cmdline);
    for (int i = pos + 1; i < njobs; ++i) joblist[i - 1] = joblist[i];
    njobs--;
}

static void jobs_move_to_end(int pos) {
    if (pos < 0 || pos >= njobs) return;
    Job tmp = joblist[pos];
    for (int i = pos + 1; i < njobs; ++i) joblist[i - 1] = joblist[i];
    joblist[njobs - 1] = tmp;
}

/* ============ small helpers ============ */
static const char* base_of(const char* p) {
    if (!p || !*p) return p;
    if (strcmp(p, "/") == 0) return "/";
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void print_prompt(void) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/");
    printf("[nyush %s]$ ", base_of(cwd));
    fflush(stdout);
}

static int is_blank(const char* s) {
    while (*s==' ' || *s=='\t' || *s=='\n') ++s;
    return *s=='\0';
}
static int is_number(const char* s){
    if(!s || !*s) return 0;
    for(const char* p=s; *p; ++p) if(*p<'0'||*p>'9') return 0;
    return 1;
}

/* ============ tokenization & parsing ============ */
typedef struct Tokens { char *v[MAX_TOK]; int n; } Tokens;

static void tokenize(char *line, Tokens *toks) {
    toks->n = 0;
    char *save=NULL;
    for (char *p=strtok_r(line, " \t\n", &save); p; p=strtok_r(NULL, " \t\n", &save)) {
        toks->v[toks->n++] = p;
        if (toks->n >= MAX_TOK) break;
    }
}

static Cmd* cmd_new(void) {
    Cmd *c = (Cmd*)calloc(1, sizeof(Cmd));
    c->argc = 0; c->append=0; c->next=NULL; c->infile=NULL; c->outfile=NULL;
    return c;
}

static Cmd* parse_pipeline(Tokens *toks) {
    if (toks->n==0) return NULL;

    Cmd *head = cmd_new(), *cur=head;
    int out_on_nonlast = 0;

    for (int i=0;i<toks->n;i++){
        char *tk=toks->v[i];
        if (strcmp(tk,"|")==0){
            if (cur->argc==0 || cur->outfile){ fprintf(stderr, ERR_INVALID_CMD); goto fail; }
            cur->next=cmd_new(); cur=cur->next; continue;
        }
        if (strcmp(tk,"<")==0){
            if (cur!=head || cur->infile || i+1>=toks->n){ fprintf(stderr, ERR_INVALID_CMD); goto fail; }
            char *fn=toks->v[++i];
            if (!strcmp(fn,"|")||!strcmp(fn,"<")||!strcmp(fn,">")||!strcmp(fn,">>")) { fprintf(stderr, ERR_INVALID_CMD); goto fail; }
            cur->infile=fn; continue;
        }
        if (!strcmp(tk,">")||!strcmp(tk,">>")){
            if (cur->outfile || i+1>=toks->n){ fprintf(stderr, ERR_INVALID_CMD); goto fail; }
            char *fn=toks->v[++i];
            if (!strcmp(fn,"|")||!strcmp(fn,"<")||!strcmp(fn,">")||!strcmp(fn,">>")) { fprintf(stderr, ERR_INVALID_CMD); goto fail; }
            cur->outfile=fn; cur->append=(tk[1]=='>');
            for (int j=i+1;j<toks->n;j++) if(!strcmp(toks->v[j],"|")){ out_on_nonlast=1; break; }
            if (out_on_nonlast){ fprintf(stderr, ERR_INVALID_CMD); goto fail; }
            continue;
        }
        if (cur->argc>=MAX_ARGS-1){ fprintf(stderr, ERR_INVALID_CMD); goto fail; }
        cur->argv[cur->argc++]=tk; cur->argv[cur->argc]=NULL;
    }
    for (Cmd*c=head;c;c=c->next) if (c->argc==0){ fprintf(stderr, ERR_INVALID_CMD); goto fail; }
    return head;
fail:
    while (head){ Cmd*n=head->next; free(head); head=n; }
    return (Cmd*)-1;
}

/* ============ program path resolution ============ */
static int is_absolute(const char*s){ return s && s[0]=='/'; }
static int contains_slash(const char*s){ return s && strchr(s,'/'); }

static int locate_program(const char* name, char *buf, size_t bufsz) {
    if (is_absolute(name) || contains_slash(name)) {
        snprintf(buf, bufsz, "%s", name);
        return access(buf, X_OK)==0 ? 0 : -1;
    } else {
        snprintf(buf, bufsz, "/usr/bin/%s", name);
        return access(buf, X_OK)==0 ? 0 : -1;
    }
}

/* ============ running pipeline ============ */
// return 1 if single process got stopped and pushed to jobs; else 0
static int run_pipeline(Cmd *head, const char *raw_cmdline) {
    if (head->infile && access(head->infile, R_OK)!=0) {
        fprintf(stderr, ERR_INVALID_FILE);
        return 0;
    }

    int n=0; for(Cmd*c=head;c;c=c->next) n++;
    int pipes[n>0?n-1:0][2];
    for(int i=0;i<n-1;i++) if (pipe(pipes[i])<0){ perror("pipe"); return 0; }

    pid_t pids[n]; memset(pids,0,sizeof(pids));

    int idx=0;
    for (Cmd*c=head;c;c=c->next,idx++){
        pid_t pid=fork();
        if (pid<0){ perror("fork"); return 0; }
        if (pid==0){
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);

            if (idx==0 && head->infile){
                int fd=open(head->infile,O_RDONLY);
                if (fd<0){ fprintf(stderr, ERR_INVALID_FILE); _exit(1); }
                dup2(fd,STDIN_FILENO); close(fd);
            } else if (idx>0){
                dup2(pipes[idx-1][0],STDIN_FILENO);
            }

            if (idx==n-1){
                if (c->outfile){
                    int fd = c->append
                        ? open(c->outfile,O_WRONLY|O_CREAT|O_APPEND,0666)
                        : open(c->outfile,O_WRONLY|O_CREAT|O_TRUNC,0666);
                    if (fd<0){ _exit(1); }
                    dup2(fd,STDOUT_FILENO); close(fd);
                }
            } else {
                dup2(pipes[idx][1],STDOUT_FILENO);
            }

            for(int k=0;k<n-1;k++){ close(pipes[k][0]); close(pipes[k][1]); }

            char prog[4096];
            if (locate_program(c->argv[0],prog,sizeof(prog))!=0){
                fprintf(stderr, ERR_INVALID_PROG);
                _exit(127);
            }
            execv(prog,c->argv);
            fprintf(stderr, ERR_INVALID_PROG);
            _exit(127);
        } else {
            pids[idx]=pid;
        }
    }
    for(int k=0;k<n-1;k++){ close(pipes[k][0]); close(pipes[k][1]); }

    int stopped_pid = 0, done=0;
    while (done<n){
        int status=0;
        pid_t w=waitpid(-1,&status,WUNTRACED);
        if (w<0){ if(errno==EINTR) continue; else break; }
        if (WIFSTOPPED(status)){ stopped_pid=w; done++; }
        else if (WIFEXITED(status)||WIFSIGNALED(status)){ done++; }
    }

    if (n==1 && stopped_pid>0){
        jobs_push(stopped_pid, raw_cmdline);   // 这里保存“完整原始命令行”
        return 1;
    }
    return 0;
}

/* ============ builtins ============ */
static int builtin_cd(Cmd *c){
    if (c->argc!=2){ fprintf(stderr, ERR_INVALID_CMD); return 0; }
    if (chdir(c->argv[1])!=0){ fprintf(stderr, ERR_INVALID_DIR); }
    return 0;
}
static int builtin_exit(Cmd *c){
    if (c->argc!=1){ fprintf(stderr, ERR_INVALID_CMD); return 0; }
    if (jobs_count()>0){ fprintf(stderr, ERR_SUSP_JOBS); return 0; }
    exit(0);
}
static int builtin_jobs(Cmd *c){
    if (c->argc!=1){ fprintf(stderr, ERR_INVALID_CMD); return 0; }
    jobs_print(); return 0;
}
static int builtin_fg(Cmd *c){
    if (c->argc!=2 || !is_number(c->argv[1])){ fprintf(stderr, ERR_INVALID_CMD); return 0; }
    int pos = jobs_index_from_print_index(atoi(c->argv[1]));
    if (pos<0){ fprintf(stderr, ERR_INVALID_JOB); return 0; }

    pid_t pid = joblist[pos].pid;
    kill(pid, SIGCONT);

    int status=0;
    pid_t w = waitpid(pid, &status, WUNTRACED);
    if (w<0) return 0;

    if (WIFSTOPPED(status)) {
        jobs_move_to_end(pos);   // 再次被暂停 -> 移到列表末尾
    } else {
        jobs_remove_at(pos);     // 前台结束 -> 从列表删除
    }
    return 0;
}
static int is_builtin_name(const char* s){
    return !strcmp(s,"cd") || !strcmp(s,"exit") || !strcmp(s,"jobs") || !strcmp(s,"fg");
}

/* ============ main loop ============ */
int main(void){
    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    char *line=NULL; size_t cap=0;

    for(;;){
        print_prompt();
        ssize_t n=getline(&line,&cap,stdin);
        if (n==-1){ putchar('\n'); break; }
        if (n>0 && line[n-1]=='\n') line[n-1]='\0';

        if (is_blank(line)) continue;

        /* 关键：在 tokenize 之前，保存“完整原始命令行” */
        char *raw = strdup(line);
        if (!raw) raw = line;  // 退化安全

        Tokens toks; tokenize(line,&toks);
        Cmd *head=parse_pipeline(&toks);
        if (head==(Cmd*)-1) { free(raw==line?NULL:raw); continue; }
        if (!head)          { free(raw==line?NULL:raw); continue; }

        if (head->next==NULL && is_builtin_name(head->argv[0])
            && !head->infile && !head->outfile)
        {
            if (!strcmp(head->argv[0],"cd"))   builtin_cd(head);
            else if (!strcmp(head->argv[0],"exit")) builtin_exit(head);
            else if (!strcmp(head->argv[0],"jobs")) builtin_jobs(head);
            else if (!strcmp(head->argv[0],"fg"))   builtin_fg(head);
            while (head){ Cmd*n=head->next; free(head); head=n; }
            free(raw==line?NULL:raw);
            continue;
        } else if (is_builtin_name(head->argv[0]) && (head->next || head->infile || head->outfile)) {
            fprintf(stderr, ERR_INVALID_CMD);
            while (head){ Cmd*n=head->next; free(head); head=n; }
            free(raw==line?NULL:raw);
            continue;
        }

        /* 运行时，用 raw（未被 strtok 破坏的整行）保存到 jobs */
        run_pipeline(head, raw);

        while (head){ Cmd*n=head->next; free(head); head=n; }
        free(raw==line?NULL:raw);
    }
    free(line);
    return 0;
}
