#define _POSIX_C_SOURCE 200809L

#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

/*		DIRECTORY and ID GENERATION HELPERS		*/

//  recursively creates directory path if it doesn't already exists
static int ensure_dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    if (stat(".claude", &st) != 0) {
        if (mkdir(".claude", 0755) != 0 && errno != EEXIST) {
            return -1;
        }
    }

    if (stat(".claude/sessions", &st) != 0) {
        if (mkdir(".claude/sessions", 0755) != 0 && errno != EEXIST) {
            return -1;
        }
    }

    return 0;
}


// generates an alphanumeric pseudo-random ID based on timestamp and process id
static void generate_session_id(char *out_id, size_t max_len) {

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    pid_t pid = getpid();
    unsigned long seed = (unsigned long)ts.tv_sec ^ ((unsigned long)ts.tv_nsec << 16) ^ (unsigned long)pid;

    snprintf(out_id, max_len, "%lx%06lx", (unsigned long)ts.tv_sec, (unsigned long)(seed & 0xFFFFFF));
}

/*		FINIDNG LATEST SESSION		*/

int session_find_latest_id(char *out_id, size_t max_len) {
	
	if (!out_id || max_len == 0) return 0;
	
	DIR *dir = opendir(SESSION_DIR);
	if (!dir) return 0;
	
	struct dirent *entry;
    time_t latest_mtime = 0;
    char latest_name[SESSION_ID_MAX] = "";
    
    while ((entry = readdir(dir)) != NULL) {
    	
    	if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
    		continue;
    	}
    	
    	// must end with .jsonl
    	char *ext = strrchr(entry->d_name, '.');
    	if (!ext || strcmp(ext, ".jsonl") != 0) {
    		continue;
    	}
    	
    	char full_path[SESSION_PATH_MAX];
    	snprintf(full_path, sizeof(full_path), "%s/%s", SESSION_DIR, entry->d_name);
    	
    	struct stat st;
    	if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
    		if (st.st_mtime >= latest_mtime) {
    			latest_mtime = st.st_mtime;
    			
    			// copy entry name without .jsonl extension
    			size_t id_len = (size_t)(ext - entry->d_name);
    			
    			if (id_len < sizeof(latest_name)) {
    				memcpy(latest_name, entry->d_name, id_len);
    				latest_name[id_len] = '\0';
    			}
    		}
    	}
    }
    closedir(dir);
    
    if (latest_name[0] == '\0') {
    	return 0;
    }
    
    snprintf(out_id, max_len, "%s", latest_name);
    return 1;
}



/*		PUBLIC INTERFACE IMPLEMETATION		*/

Session *session_open(const char *session_id, int continue_latest) {
    if (ensure_dir_exists(SESSION_DIR) != 0) {
        fprintf(stderr, "[session] ensure_dir_exists(\"%s\") failed: %s\n", SESSION_DIR, strerror(errno));
        return NULL;
    }

    Session *s = calloc(1, sizeof(Session));
    if (!s) {
        fprintf(stderr, "[session] calloc failed: %s\n", strerror(errno));
        return NULL;
    }

    char resolved_id[SESSION_ID_MAX] = "";

    if (continue_latest) {
        if (!session_find_latest_id(resolved_id, sizeof(resolved_id))) {
            generate_session_id(resolved_id, sizeof(resolved_id));
            s->is_resumed = 0;
        } else {
            s->is_resumed = 1;
        }
    } else if (session_id && *session_id) {
        snprintf(resolved_id, sizeof(resolved_id), "%s", session_id);
        char test_path[SESSION_PATH_MAX];
        snprintf(test_path, sizeof(test_path), "%s/%s.jsonl", SESSION_DIR, resolved_id);
        if (access(test_path, F_OK) == 0) {
            s->is_resumed = 1;
        } else {
            s->is_resumed = 0;
        }
    } else {
        generate_session_id(resolved_id, sizeof(resolved_id));
        s->is_resumed = 0;
    }

    snprintf(s->id, sizeof(s->id), "%s", resolved_id);
    snprintf(s->file_path, sizeof(s->file_path), "%s/%s.jsonl", SESSION_DIR, s->id);
    s->created_at = time(NULL);

    s->file = fopen(s->file_path, "a+");
    if (!s->file) {
        fprintf(stderr, "[session] fopen failed for \"%s\": %s\n", s->file_path, strerror(errno));
        free(s);
        return NULL;
    }

    if (!s->is_resumed) {
        cJSON *init_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(init_obj, "type", "session_init");
        cJSON_AddStringToObject(init_obj, "id", s->id);
        cJSON_AddNumberToObject(init_obj, "created_at", (double)s->created_at);

        char *line = cJSON_PrintUnformatted(init_obj);
        cJSON_Delete(init_obj);

        if (line) {
            fprintf(s->file, "%s\n", line);
            fflush(s->file);
            free(line);
        }
    }

    return s;
}


int session_load_history(Session *s, cJSON *messages) {

    if (!s || !messages) return 0;

    FILE *f = fopen(s->file_path, "r");
    if (!f) return 0;

    char *line = NULL;
    size_t len = 0;
    ssize_t read_bytes;
    int loaded_count = 0;

    while ((read_bytes = getline(&line, &len, f)) != -1) {
        // Strip trailing newline characters
        while (read_bytes > 0 && (line[read_bytes - 1] == '\n' || line[read_bytes - 1] == '\r')) {
            line[--read_bytes] = '\0';
        }
        if (read_bytes == 0) continue;

        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        cJSON *type = cJSON_GetObjectItem(entry, "type");
        if (type && cJSON_IsString(type) && strcmp(type->valuestring, "session_init") == 0) {
            // Skip metadata header line
            cJSON_Delete(entry);
            continue;
        }

        // Reconstruct message object for OpenAI/OpenRouter payload
        cJSON *msg = cJSON_CreateObject();
        cJSON *role = cJSON_GetObjectItem(entry, "role");
        if (role && cJSON_IsString(role)) {
            cJSON_AddStringToObject(msg, "role", role->valuestring);
        }

        cJSON *content = cJSON_GetObjectItem(entry, "content");
        if (content && cJSON_IsString(content)) {
            cJSON_AddStringToObject(msg, "content", content->valuestring);
        } else if (content && cJSON_IsNull(content)) {
            cJSON_AddNullToObject(msg, "content");
        }

        cJSON *tool_calls = cJSON_GetObjectItem(entry, "tool_calls");
        if (tool_calls && cJSON_IsArray(tool_calls)) {
            cJSON_AddItemToObject(msg, "tool_calls", cJSON_Duplicate(tool_calls, 1));
        }

        cJSON *tool_call_id = cJSON_GetObjectItem(entry, "tool_call_id");
        if (tool_call_id && cJSON_IsString(tool_call_id)) {
            cJSON_AddStringToObject(msg, "tool_call_id", tool_call_id->valuestring);
        }

        cJSON_AddItemToArray(messages, msg);
        loaded_count++;

        cJSON_Delete(entry);
    }

    if (line) free(line);
    fclose(f);
    return loaded_count;
}

int session_append_message(Session *s, const cJSON *message) {

    if (!s || !s->file || !message) return -1;

    // Wrap the message in our JSONL entry representation
    cJSON *entry = cJSON_Duplicate(message, 1);
    if (!entry) return -1;

    // Ensure entry has type="message"
    if (!cJSON_GetObjectItem(entry, "type")) {
        cJSON_AddStringToObject(entry, "type", "message");
    }

    char *rendered = cJSON_PrintUnformatted(entry);
    cJSON_Delete(entry);
    if (!rendered) return -1;

    int res = fprintf(s->file, "%s\n", rendered);
    fflush(s->file);
    free(rendered);

    return (res > 0) ? 0 : -1;
}

void session_close(Session *s) {

    if (!s) return;
    if (s->file) {
        fflush(s->file);
        fclose(s->file);
        s->file = NULL;
    }
    free(s);
}

// Appends an assistant message containing tool_calls to the session JSONL
void session_log_assistant_tool_calls(Session *sess, cJSON *tool_calls_array, const char *optional_content) {

    if (!sess || !sess->file || !tool_calls_array) return;

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "message");
    cJSON_AddStringToObject(obj, "role", "assistant");

    if (optional_content && strlen(optional_content) > 0) {
        cJSON_AddStringToObject(obj, "content", optional_content);
    } else {
        cJSON_AddNullToObject(obj, "content");
    }

    // Duplicate the tool_calls cJSON array into the log object
    cJSON_AddItemToObject(obj, "tool_calls", cJSON_Duplicate(tool_calls_array, 1));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        fprintf(sess->file, "%s\n", line);
        fflush(sess->file);
        free(line);
    }
}

// Appends a tool execution result to the session JSONL
void session_log_tool_result(Session *sess, const char *tool_call_id, const char *tool_name, const char *result_content) {

    if (!sess || !sess->file) return;

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "message");
    cJSON_AddStringToObject(obj, "role", "tool");
    cJSON_AddStringToObject(obj, "tool_call_id", tool_call_id ? tool_call_id : "");
    
    if (tool_name) {
        cJSON_AddStringToObject(obj, "name", tool_name);
    }
    cJSON_AddStringToObject(obj, "content", result_content ? result_content : "");

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        fprintf(sess->file, "%s\n", line);
        fflush(sess->file);
        free(line);
    }
}

// Prints all existing sessions with modification timestamps and line counts
void session_list_all(void) {

    DIR *dir = opendir(SESSION_DIR);
    if (!dir) {
        printf("No sessions found (directory '%s' does not exist).\n", SESSION_DIR);
        return;
    }

    struct dirent *entry;
    int count = 0;
    printf("%-24s %-22s %s\n", "SESSION ID", "LAST MODIFIED", "TURNS");
    printf("----------------------------------------------------------------\n");

    while ((entry = readdir(dir)) != NULL) {
        char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".jsonl") != 0) continue;

        char full_path[SESSION_PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", SESSION_DIR, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            // Count turns (non-empty lines minus init)
            FILE *f = fopen(full_path, "r");
            int lines = 0;
            if (f) {
                char buf[512];
                while (fgets(buf, sizeof(buf), f)) {
                    if (strlen(buf) > 1) lines++;
                }
                fclose(f);
            }
            int turns = lines > 1 ? lines - 1 : 0;

            // Session ID without .jsonl
            size_t id_len = (size_t)(ext - entry->d_name);
            char id_buf[SESSION_ID_MAX] = "";
            if (id_len < sizeof(id_buf)) {
                memcpy(id_buf, entry->d_name, id_len);
                id_buf[id_len] = '\0';
            }

            char time_buf[64];
            struct tm *tm_info = localtime(&st.st_mtime);
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

            printf("%-24s %-22s %d\n", id_buf, time_buf, turns);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        printf("No active session files found.\n");
    }
}

// Checks if a given session file exists
int session_exists(const char *session_id) {
    if (!session_id || !*session_id) return 0;
    char path[SESSION_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.jsonl", SESSION_DIR, session_id);
    return (access(path, F_OK) == 0);
}
