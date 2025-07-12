#include <pwd.h>
#include <grp.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <stdbool.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include "ansi.h"
#include "config.h"

struct termios old, new;

struct selected_entry {
    char path[1024];
    char name[256];
    int type;
};

struct state {
    struct dirent *current_entry;
    struct selected_entry selected_entries[256];

    char search_string[1024];
    char workspaces[9][1024];

    int current_index;
    int workspace_index; 
    int selected_count;

    bool show_hidden;
    bool more_info;
};

bool is_executable(int mode) {
    return S_ISREG(mode) && ((mode & S_IXUSR) || (mode & S_IXGRP) || (mode & S_IXOTH));
}

bool is_dir(char *path) {
    struct stat path_stat;
    stat(path, &path_stat);

    return S_ISDIR(path_stat.st_mode);
}

char *get_abs_path(char *path) {
    char *abs_path = realpath(path, NULL);
    return abs_path;
}

char get_file_type_char(int type) {
    switch(type) {
        case DT_REG:
            return '-';
        case DT_DIR:
            return 'd';
        case DT_LNK:
            return 'l';
        case DT_BLK:
            return 'b';
        case DT_CHR:
            return 'c';
        case DT_SOCK:
            return 's';
        case DT_FIFO:
            return 'p';
        default:
            return '?';
    }
}

char *get_readable_perms(char *perms_buff, int mode, int type) {
    perms_buff[0] = get_file_type_char(type);
    perms_buff[1] = (mode & S_IRUSR) ? 'r' : '-';
    perms_buff[2] = (mode & S_IWUSR) ? 'w' : '-';
    perms_buff[3] = (mode & S_IXUSR) ? 'x' : '-';
    perms_buff[4] = (mode & S_IRGRP) ? 'r' : '-';
    perms_buff[5] = (mode & S_IWGRP) ? 'w' : '-';
    perms_buff[6] = (mode & S_IXGRP) ? 'x' : '-';
    perms_buff[7] = (mode & S_IROTH) ? 'r' : '-';
    perms_buff[8] = (mode & S_IWOTH) ? 'w' : '-';
    perms_buff[9] = (mode & S_IXOTH) ? 'x' : '-';
    perms_buff[10] = '\0';

    return perms_buff;
}

char *get_readable_size(char *size_buff, long bytes) {
    char *postfix[] = { "B", "K", "M", "G", "T" };

    double dbytes = (double)bytes;

    for (int i = 0; i < 5; i++) {
        if (dbytes / 1024 < 1) {
            snprintf(size_buff, 32, dbytes == bytes ? "%.0f%s" : "%.1f%s", dbytes, postfix[i]);
            break;
        }

        dbytes /= 1024;
    }

    return size_buff;
}

bool get_input(char *input) {
    printf(SHOW_CURSOR);
    tcsetattr(0, TCSANOW, &old);
    fgets(input, 1024, stdin);
    tcsetattr(0, TCSANOW, &new);
    printf(HIDE_CURSOR);

    if (strcmp(input, "\n") == 0)
        return false;

    return true;
}

void select_entry(struct state *state) {
    bool deselect = false;

    struct selected_entry *selected_entry;

    for (int i = 0; i < state->selected_count; i++) {
        if (strcmp(state->selected_entries[i].name, state->current_entry->d_name) == 0) {
            deselect = true;
            continue;
        }

        if (deselect) {
           state->selected_entries[i - 1] = state->selected_entries[i];
        }
    }

    if (deselect) {
        state->selected_count--;
        return;
    }

    selected_entry = &state->selected_entries[state->selected_count];

    getcwd(selected_entry->path, 1024);
    strlcpy(selected_entry->name, state->current_entry->d_name, 256);
    selected_entry->type = state->current_entry->d_type;

    state->selected_count++;
}

int get_entries(struct state *state, bool subdir) {
    struct passwd *passwd;
    struct group *group;

    struct dirent *entry;
   
    struct stat entry_stat;
    struct winsize w;

    DIR *dir;

    char cwd[1024];
    char search_buff[1024];
    char link_origin[1024];

    char size_buff[32];

    char perms_buff[11];

    ioctl(0, TIOCGWINSZ, &w);

    int entry_count = 0;
    int offset = 0;
    int rows = w.ws_row - 8;

    unsigned long search_string_len = strlen(state->search_string);

    dir = opendir(".");

    getcwd(cwd, 1024);

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        if (strlen(state->search_string) > 0) {
            strlcpy(search_buff, state->search_string + 1, 1024);

            switch (state->search_string[0]) {
                case '!':
                    if (strlen(entry->d_name) < search_string_len - 1)
                        break;
                    
                    if (strncasecmp(entry->d_name, search_buff, search_string_len - 1) == 0) {
                        state->current_entry = entry;
                        select_entry(state);
                    }
                    break;
                case '?':
                    if (strlen(entry->d_name) < search_string_len - 1)
                        break;

                    if (strcasecmp(entry->d_name + (strlen(entry->d_name) - search_string_len + 1), search_buff) == 0) {
                        state->current_entry = entry;
                        select_entry(state);
                    }

                    break;    
                case '*':
                     if (strcasestr(entry->d_name, search_buff) != NULL) {
                        state->current_entry = entry;
                        select_entry(state);
                     }

                     break;
                default:
                    if (strcasestr(entry->d_name, state->search_string) != NULL) {
                        state->current_index = entry_count;
                        search_string_len = 0;
                    }
                    break;
            }
        }    

        if (state->current_index > (offset + 1) * rows) {
            offset++;
        } else if (state->current_index < offset * rows) {
            offset--;
        }

        if (entry_count < offset * rows || entry_count > (offset + 1) * rows) {
            entry_count++;
            continue;
        }

        if (!state->show_hidden) {
            if (entry->d_name[0] == '.') {
                continue;
            }
        }

        stat(entry->d_name, &entry_stat);

        if (subdir) {
            printf("\033[%d;%dH", 5 + entry_count, w.ws_col / 2);
        } else {
            get_readable_perms(perms_buff, entry_stat.st_mode, entry->d_type);
            get_readable_size(size_buff, entry_stat.st_size);

            printf("%s %s", perms_buff, size_buff);
            
            for (int i = strlen(size_buff); i < 8; i++) {
                putchar(' ');
            }

            if (state->more_info) {
                passwd = getpwuid(entry_stat.st_uid);
                group = getgrgid(entry_stat.st_gid);
                printf("%s   %s\t", passwd->pw_name, group->gr_name);
            } 
        }
        
        if (entry_count == state->current_index && !search_string_len && !subdir) {
            state->current_entry = entry;

            printf("\0337");

            if (is_dir(entry->d_name)) {
                chdir(entry->d_name);
                get_entries(state, true);
                chdir("..");
            }

            printf("\0338");
            printf(BOLD "-> ");
        }

        for (int i = 0; i < state->selected_count; i++) {
            if (strcmp(state->selected_entries[i].name, entry->d_name) == 0 &&
                strcmp(state->selected_entries[i].path, cwd) == 0) {
                printf(WHITE_BG);
                break;
            }
        }

        switch(entry->d_type) {
            case DT_DIR:
                printf(BLUE_FG);
                break;
            case DT_LNK:
                printf(CYAN_FG);
                break;
            case DT_BLK:
            case DT_CHR:
            case DT_FIFO:
            case DT_SOCK:
                printf(YELLOW_FG);
                break;    
        }

        if (is_executable(entry_stat.st_mode)) {
            printf(GREEN_FG);
        }

        printf("%s", entry->d_name);

        if (entry->d_type == DT_LNK) {
            size_t read = readlink(entry->d_name, link_origin, 1024);
            link_origin[read] = '\0';

            printf(" [%s]", link_origin);
        }

        printf("\n" RESET);

        entry_count++;
    }

    closedir(dir);

    printf("\n");

    return entry_count;
}

void enter_dir(struct state *state) {
    bool valid_dir = false;

    char link_origin[1024];
    char dir_path[1024];

    if (state->current_entry->d_type == DT_DIR) {
        valid_dir = true;
        strlcpy(dir_path, state->current_entry->d_name, 1024);
    } else if (state->current_entry->d_type == DT_LNK) {
        size_t read = readlink(state->current_entry->d_name, link_origin, 1024);
        link_origin[read] = '\0';
                    
        if (is_dir(link_origin)) {
            strlcpy(dir_path, link_origin, 1024);
            valid_dir = true;
        }
    }

    if (valid_dir) {
        chdir(dir_path);
        getcwd(state->workspaces[state->workspace_index], 1024);
    }
}

void leave_dir(struct state *state) {
    chdir("..");
    getcwd(state->workspaces[state->workspace_index], 1024);
}

void change_workspace(struct state *state) {
    char *workspace = state->workspaces[state->workspace_index];

     if (workspace[0] == '\0') {
        getcwd(workspace, 1024);
    }

    chdir(workspace);
}

void copy_file(char *source_path, char *dest_path) {
    FILE *source, *dest;
    struct stat source_stat;
    int c;

    stat(source_path, &source_stat);

    source = fopen(source_path, "r");
    dest = fopen(dest_path, "w");
        
    while ((c = fgetc(source)) != EOF) {
        fputc(c, dest);
    }

    stat(source_path, &source_stat);
    fchmod(fileno(dest), source_stat.st_mode);

    fclose(source);
    fclose(dest);
}

void copy_dir(char *dir_name_origin, char *dir_path_origin, char *dir_path) {    
    struct dirent *entry;
    struct stat source_stat;

    char link_origin[1024];
    char abs_entry_name[1024];

    DIR *dir = opendir(dir_path);

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        snprintf(abs_entry_name, 1024, "%s/%s", dir_path, entry->d_name);
        char *stripped_entry = abs_entry_name + strlen(dir_path_origin) - strlen(dir_name_origin);

        stat(abs_entry_name, &source_stat);

        if (entry->d_type == DT_DIR) {
            mkdir(stripped_entry, source_stat.st_mode);
            copy_dir(dir_name_origin, dir_path_origin, abs_entry_name);
        } else if (entry->d_type == DT_REG) {
            copy_file(abs_entry_name, stripped_entry);
        } else if (entry->d_type == DT_LNK) {
            size_t read = readlink(abs_entry_name, link_origin, 1024);
            link_origin[read] = '\0';
            symlink(link_origin, stripped_entry);
        }
    }

    closedir(dir);
}

void copy_entry(struct state *state) {
    struct stat source_stat;
   
    char link_origin[1024];
    char source_path[1280];
    
    for (int i = 0; i < state->selected_count; i++) {
        snprintf(source_path, 1280, "%s/%s", 
            state->selected_entries[i].path,
            state->selected_entries[i].name
        );

        if (state->selected_entries[i].type == DT_REG) {
            copy_file(source_path, state->selected_entries[i].name);
        } else if (state->selected_entries[i].type == DT_DIR) {
            stat(source_path, &source_stat);
            mkdir(state->selected_entries[i].name, source_stat.st_mode);
            copy_dir(state->selected_entries[i].name, source_path, source_path);
        } else if (state->selected_entries[i].type == DT_LNK) {
            size_t read = readlink(source_path, link_origin, 1024);
            link_origin[read] = '\0';
            symlink(link_origin, state->selected_entries[i].name);
        }
    }
}

void move_entry(struct state *state) {
    char source_path[1280];
    char indexed_name[1290];
    char name[1280];

    if (state->selected_count == 0)
        return;

    bool custom_name = false;

    printf("New name: ");
    get_input(name);

    if (strcmp(name, "\n") != 0) {
        name[strcspn(name, "\n")] = 0;
        custom_name = true;
    }

    for (int i = 0; i < state->selected_count; i++) {
        snprintf(source_path, 1280, "%s/%s", 
            state->selected_entries[i].path,
            state->selected_entries[i].name
        );

        if (custom_name) {
            if (i > 0) {
                snprintf(indexed_name, 1290, "%d%s", i, name);
                strlcpy(name, indexed_name, 1280);
            }
        } else {
            strlcpy(name, state->selected_entries[i].name, 1280);
        }

        rename(source_path, name);
    }

    state->selected_count = 0;
}

void symlink_entry(struct state *state) {
    char source_path[1280];
    
    for (int i = 0; i < state->selected_count; i++) {
        snprintf(source_path, 1280, "%s/%s", 
            state->selected_entries[i].path,
            state->selected_entries[i].name
        );

        symlink(source_path, state->selected_entries[i].name);
    }
}

void chmod_entry(struct state *state) {
    char perms[1024];
    char source_path[1280];

    if (state->selected_count == 0)
        return;

    printf("Permissions: ");
    if (!get_input(perms))
        return;

    perms[strcspn(perms, "\n")] = 0;
    
    for (int i = 0; i < state->selected_count; i++) {
        snprintf(source_path, 1280, "%s/%s", 
            state->selected_entries[i].path,
            state->selected_entries[i].name
        );

        chmod(source_path, S_IFREG | strtol(perms, NULL, 8));
    }
}

void open_dir(struct state *state) {
    char path[1024];
    char temp_path[1024];

    printf("Open: ");
    if (!get_input(path))
        return;

    path[strcspn(path, "\n")] = 0;

    if (path[0] == '~') {
        snprintf(temp_path, 1024, "%s%s", getenv("HOME"), path + 1);
        strlcpy(path, temp_path, 1024);
    }

    if (chdir(path) != 0)
        return;

    strlcpy(state->workspaces[state->workspace_index], path, 1024);
}

void xdg_open(struct state *state) {
    char *exec_args[3];
    pid_t pid;

    #ifdef __APPLE__
        exec_args[0] = "/usr/bin/open";
    #else
        exec_args[0] = "/usr/bin/xdg-open";
    #endif

    int type = state->current_entry->d_type;

    if (type != DT_REG && type != DT_LNK) {
        return;
    }

    exec_args[1] = state->current_entry->d_name;
    exec_args[2] = NULL;

    pid = fork();

    if (pid == 0) {
        execv(exec_args[0], exec_args);
        exit(0);
    }
}

void execute_entry(struct state *state) {
    char bin_path[1024];
    char exec_string[1024];

    char *split;
    char *exec_args[128];

    pid_t pid;

    if (state->selected_count == 0)
        return;

    printf("Execute: ");

    if (!get_input(exec_string))
        return;
    
    split = strtok(exec_string, " ");

    int i = 0;

    while (split != NULL) {
        if (split[0] == '@') {
            for (int j = 0; j < state->selected_count; j++) {
                exec_args[i] = state->selected_entries[j].name;
                i++;
            }
        } else {
            exec_args[i] = split;
            i++;
        }

        split = strtok(NULL, " ");
    }

    exec_args[i] = NULL;

    snprintf(bin_path, 1024, "/usr/bin/%s", exec_args[0]);

    pid = fork();

    waitpid(pid, NULL, WUNTRACED);

    if (pid == 0) {
        printf(SHOW_CURSOR);
        tcsetattr(0, TCSANOW, &old);
        int ret = execv(bin_path, exec_args);
        if (ret == -1) {
            execv(exec_args[0], exec_args);
        }
        exit(0);
    } else {
        printf(HIDE_CURSOR);
        tcsetattr(0, TCSANOW, &new);
    }
}

void create_file(void) {
    char name[1024];

    printf("File name: ");

    if (!get_input(name))
        return;

    name[strcspn(name, "\n")] = 0;

    creat(name, 33188); // -rw-r--r--
}

void create_dir(void) {
    char name[1024];

    printf("Directory name: ");

    if (!get_input(name))
        return;

    name[strcspn(name, "\n")] = 0;

    mkdir(name, 16877); // drwxr-xr-x
}

void remove_dir(char *dir_path) {    
    struct dirent *entry;
    struct stat source_stat;

    char abs_entry_name[1024];

    DIR *dir = opendir(dir_path);

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        snprintf(abs_entry_name, 1024, "%s/%s", dir_path, entry->d_name);

        stat(abs_entry_name, &source_stat);

        if (entry->d_type == DT_DIR) {
            remove_dir(abs_entry_name);
        }

        remove(abs_entry_name);
    }

    closedir(dir);
}

void remove_entry(struct state *state) {
    char source_path[1280];
    char choice;

    if (state->selected_count == 0)
        return;

    printf("Remove %d entries? (y/n)", state->selected_count);

    choice = getchar();

    if (choice != 'y' && choice != 'Y')
        return;

    for (int i = 0; i < state->selected_count; i++) {
        snprintf(source_path, 1280, "%s/%s", 
            state->selected_entries[i].path,
            state->selected_entries[i].name
        );

        if (state->selected_entries[i].type == DT_DIR) {
            remove_dir(source_path);
        }

        remove(source_path);
    }

    state->selected_count = 0;
}

void search_entry(struct state *state) {
    char name[1024];

    printf("Searching for: ");

    if (!get_input(name))
        return;

    name[strcspn(name, "\n")] = 0;

    strlcpy(state->search_string, name, 1024);
}

int main(int argc, char **argv) {
    struct state state;

    char operation;
    int entry_count;

    state.current_index = 0;
    state.workspace_index = 0; 
    state.selected_count = 0;
    state.show_hidden = false;
    state.more_info = false;

    for (int i = 0; i < 9; i++) {
        state.workspaces[i][0] = '\0';
    }

    tcgetattr(0, &old);
    new = old;
    new.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &new);

    if (argc == 1) {
        getcwd(state.workspaces[0], 1024);
    } else {
        strlcpy(state.workspaces[0], get_abs_path(argv[1]), 1024);
        chdir(state.workspaces[0]);
    }
   
    do {
        printf(CLEAR);
        printf(HIDE_CURSOR);

        for (int i = 0 ; i < 9; i++) {
            if (state.workspace_index == i) {
               printf(CYAN_FG);
            }
            printf("[%d] " RESET, i + 1);
        }
        
        printf("\n\n");
        printf("%s%s%s ", BOLD, state.workspaces[state.workspace_index], RESET);
        printf("[%s%s%dS%s]", BLINK, CYAN_FG, state.selected_count, RESET);
        printf("\n\n");

        entry_count = get_entries(&state, false);

        if (strlen(state.search_string) > 0) {
           state.search_string[0] = '\0';
            continue;
        }

        if (entry_count <= state.current_index && entry_count != 0) {
            state.current_index = entry_count ? entry_count - 1 : entry_count;
            continue;
        }

        operation = getchar();

        switch (operation) {
            case MOVE_UP:
                if (state.current_index == 0)
                    break;
                
                state.current_index--;
                break;

            case MOVE_DOWN:
                if (state.current_index == entry_count - 1)
                    break;
                
                state.current_index++;
                break;

            case ENTER_DIR:
                enter_dir(&state);
                break;
            
            case LEAVE_DIR:
                leave_dir(&state);
                break;

            case JUMP_FIRST:
                state.current_index = 0;
                break;

            case JUMP_LAST:
                state.current_index = entry_count - 1;
                break;    

            case 49 ... 57: // ASCII 1 ... 9
                state.workspace_index = operation - 49;
                state.current_index = 0;
                change_workspace(&state);
                break;

            case SELECT_ALL:
                strlcpy(state.search_string, "*", 2);
                break;
            
            case SELECT_ENTRY:
                select_entry(&state);
                break;
            
            case CLEAR_SELECT:
                state.selected_count = 0;
                break;

            case COPY:
                copy_entry(&state);
                break;
             
            case MOVE:
                move_entry(&state);
                break;

            case SYMLINK:
                symlink_entry(&state);
                break;
            
            case CHMOD:
                chmod_entry(&state);
                break;
            
            case OPEN_DIR:
                open_dir(&state);
                break;
            
            case XDG_OPEN:
                xdg_open(&state);
                break;

            case EXECUTE:
                execute_entry(&state);
                break;

            case TOUCH:
                create_file();
                break;
            
            case MKDIR:
                create_dir();
                break;
            
            case REMOVE:
                remove_entry(&state);
                break;

             case SEARCH:
                search_entry(&state);
                break;    

            case SHOW_HIDDEN:
                state.show_hidden = !state.show_hidden;
                break;
            
            case MORE_INFO:
                state.more_info = !state.more_info;
                break;
        }

    } while (operation != EXIT);

    printf(CLEAR);
    printf(SHOW_CURSOR);

    tcsetattr(0, TCSANOW, &old);
}
