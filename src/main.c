#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

struct response_buf {
    char *data;
    size_t size;
};

static size_t curl_write_response(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct response_buf *buf = (struct response_buf *)userp;
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[read] = '\0';
    if (out_len) *out_len = read;
    return buf;
}

static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    int close_result = fclose(f);

    if (written != len || close_result != 0) return -1;
    return 0;
}

static void add_tool(cJSON *tools, const char *name, const char *description,
                     const char *params[][2], int param_count) {
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");

    cJSON *function = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(function, "name", name);
    cJSON_AddStringToObject(function, "description", description);

    cJSON *parameters = cJSON_AddObjectToObject(function, "parameters");
    cJSON_AddStringToObject(parameters, "type", "object");

    cJSON *properties = cJSON_AddObjectToObject(parameters, "properties");
    cJSON *required = cJSON_AddArrayToObject(parameters, "required");

    for (int i = 0; i < param_count; i++) {
        cJSON *prop = cJSON_AddObjectToObject(properties, params[i][0]);
        cJSON_AddStringToObject(prop, "type", "string");
        cJSON_AddStringToObject(prop, "description", params[i][1]);
        cJSON_AddItemToArray(required, cJSON_CreateString(params[i][0]));
    }

    cJSON_AddItemToArray(tools, tool);
}

static cJSON *build_tools(void) {
    cJSON *tools = cJSON_CreateArray();

    const char *read_params[][2] = {
        {"file_path", "The path to the file to read"},
    };
    add_tool(tools, "Read", "Read and return the contents of a file", read_params, 1);

    const char *write_params[][2] = {
        {"file_path", "The path of the file to write to"},
        {"content", "The content to write to the file"},
    };
    add_tool(tools, "Write", "Write content to a file", write_params, 2);

    const char *bash_params[][2] = {
        {"command", "The command to run"},
    };
    add_tool(tools, "Bash", "Execute a shell command", bash_params, 1);

    return tools;
}

// Substitutes $ARGUMENTS, $ARGUMENTS[i], and $i placeholders in template.
// Caller must free the returned string.
static char *substitute_placeholders(const char *template, const char *raw_args) {
    if (!template) return NULL;

    // 1. Trim raw arguments string ($ARGUMENTS)
    char full_args[1024] = "";
    if (raw_args) {
        while (isspace((unsigned char)*raw_args)) raw_args++;
        strncpy(full_args, raw_args, sizeof(full_args) - 1);
        full_args[sizeof(full_args) - 1] = '\0';
        // Trim trailing space
        size_t fa_len = strlen(full_args);
        while (fa_len > 0 && isspace((unsigned char)full_args[fa_len - 1])) {
            full_args[--fa_len] = '\0';
        }
    }

    // 2. Tokenize positional arguments ($0, $1, ...)
    char *args_tokens[64];
    int arg_count = 0;
    char args_copy[1024];
    strncpy(args_copy, full_args, sizeof(args_copy) - 1);
    args_copy[sizeof(args_copy) - 1] = '\0';

    char *token = strtok(args_copy, " \t\r\n");
    while (token && arg_count < 64) {
        args_tokens[arg_count++] = token;
        token = strtok(NULL, " \t\r\n");
    }

    // 3. Scan and build the substituted string buffer
    size_t cap = strlen(template) + strlen(full_args) * 2 + 512;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t out_len = 0;

    const char *p = template;
    while (*p) {
        if (*p == '$') {
            // Check for $ARGUMENTS[...] or $ARGUMENTS
            if (strncmp(p, "$ARGUMENTS", 10) == 0) {
                if (p[10] == '[') {
                    // $ARGUMENTS[index]
                    char *end_bracket = strchr(p + 11, ']');
                    if (end_bracket) {
                        int idx = atoi(p + 11);
                        const char *rep = (idx >= 0 && idx < arg_count) ? args_tokens[idx] : "";
                        size_t rep_len = strlen(rep);

                        while (out_len + rep_len + 1 > cap) {
                            cap *= 2;
                            char *tmp = realloc(out, cap);
                            if (!tmp) { free(out); return NULL; }
                            out = tmp;
                        }
                        memcpy(out + out_len, rep, rep_len);
                        out_len += rep_len;
                        p = end_bracket + 1;
                        continue;
                    }
                } else {
                    // Plain $ARGUMENTS
                    size_t rep_len = strlen(full_args);
                    while (out_len + rep_len + 1 > cap) {
                        cap *= 2;
                        char *tmp = realloc(out, cap);
                        if (!tmp) { free(out); return NULL; }
                        out = tmp;
                    }
                    memcpy(out + out_len, full_args, rep_len);
                    out_len += rep_len;
                    p += 10;
                    continue;
                }
            }
            // Check for $0, $1, $2, ...
            else if (isdigit((unsigned char)*(p + 1))) {
                char *end_digit = NULL;
                long idx = strtol(p + 1, &end_digit, 10);
                const char *rep = (idx >= 0 && idx < arg_count) ? args_tokens[idx] : "";
                size_t rep_len = strlen(rep);

                while (out_len + rep_len + 1 > cap) {
                    cap *= 2;
                    char *tmp = realloc(out, cap);
                    if (!tmp) { free(out); return NULL; }
                    out = tmp;
                }
                memcpy(out + out_len, rep, rep_len);
                out_len += rep_len;
                p = end_digit;
                continue;
            }
        }

        // Regular character copy
        if (out_len + 2 > cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
        }
        out[out_len++] = *p++;
    }

    out[out_len] = '\0';
    return out;
}

static char *run_bash(const char *cmd) {
    char full_cmd[2048];
    snprintf(full_cmd, sizeof(full_cmd), "%s 2>&1", cmd);

    FILE *fp = popen(full_cmd, "r");
    if (!fp) {
        return strdup("error: failed to execute command");
    }

    size_t cap = 1024;
    size_t len = 0;
    char *output = malloc(cap);
    if (!output) {
        pclose(fp);
        return strdup("error: out of memory");
    }
    output[0] = '\0';

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        size_t b_len = strlen(buffer);
        if (len + b_len + 1 > cap) {
            cap = (cap + b_len) * 2;
            char *tmp = realloc(output, cap);
            if (!tmp) {
                free(output);
                pclose(fp);
                return strdup("error: out of memory");
            }
            output = tmp;
        }
        memcpy(output + len, buffer, b_len);
        len += b_len;
        output[len] = '\0';
    }

    pclose(fp);
    return output;
}

static char *execute_tool(const char *name, const char *args_str) {
    if (!name || !args_str) {
        return strdup("error: invalid tool call");
    }

    if (strcmp(name, "Read") == 0) {
        cJSON *args = cJSON_Parse(args_str);
        const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(args, "file_path"));
        if (!path) {
            cJSON_Delete(args);
            return strdup("error: missing file_path argument");
        }

        char *contents = read_file(path, NULL);
        cJSON_Delete(args);
        if (!contents) {
            return strdup("error: could not read file");
        }
        return contents;
    }

    if (strcmp(name, "Write") == 0) {
        cJSON *args = cJSON_Parse(args_str);
        const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(args, "file_path"));
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(args, "content"));
        if (!path || !content) {
            cJSON_Delete(args);
            return strdup("error: missing file_path or content argument");
        }

        char *result;
        if (write_file(path, content) == 0) {
            size_t n = strlen(path) + 64;
            result = malloc(n);
            if (result) {
                snprintf(result, n, "Successfully wrote %zu bytes to %s", strlen(content), path);
            }
        } else {
            result = strdup("error: could not write file");
        }
        cJSON_Delete(args);
        return result;
    }

    if (strcmp(name, "Bash") == 0) {
        cJSON *args = cJSON_Parse(args_str);
        const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(args, "command"));
        if (!cmd) {
            cJSON_Delete(args);
            return strdup("error: missing command argument");
        }

        char *result = run_bash(cmd);
        cJSON_Delete(args);
        return result;
    }

    return strdup("error: unknown tool");
}

/* ================= Step 7 & 8: Skills & Frontmatter ================= */

static char *trim_whitespace(char *str) {
    if (!str) return NULL;
    while (isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;

    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static void parse_frontmatter(const char *frontmatter, char **out_name, char **out_desc) {
    *out_name = NULL;
    *out_desc = NULL;

    char *copy = strdup(frontmatter);
    if (!copy) return;

    char *line = strtok(copy, "\r\n");
    while (line) {
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char *key = trim_whitespace(line);
            char *val = trim_whitespace(colon + 1);

            if (strcmp(key, "name") == 0 && !*out_name) {
                *out_name = strdup(val);
            } else if (strcmp(key, "description") == 0 && !*out_desc) {
                *out_desc = strdup(val);
            }
        }
        line = strtok(NULL, "\r\n");
    }
    free(copy);
}

// Step 7: Discovers skills and formats the system message
static char *build_skills_system_prompt(void) {
    DIR *dir = opendir(".claude/skills");
    if (!dir) return NULL;

    size_t cap = 1024;
    char *prompt = malloc(cap);
    if (!prompt) {
        closedir(dir);
        return NULL;
    }
    strcpy(prompt, "You have access to the following skills:\n");

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char file_path[512];
        snprintf(file_path, sizeof(file_path), ".claude/skills/%s/SKILL.md", entry->d_name);

        size_t file_len = 0;
        char *content = read_file(file_path, &file_len);
        if (!content) continue;

        char *first_dash = strstr(content, "---");
        if (first_dash) {
            char *second_dash = strstr(first_dash + 3, "---");
            if (second_dash) {
                size_t fm_len = (size_t)(second_dash - (first_dash + 3));
                char *fm = malloc(fm_len + 1);
                if (fm) {
                    memcpy(fm, first_dash + 3, fm_len);
                    fm[fm_len] = '\0';

                    char *name = NULL;
                    char *desc = NULL;
                    parse_frontmatter(fm, &name, &desc);

                    const char *skill_name = name ? name : entry->d_name;
                    const char *skill_desc = desc ? desc : "";

                    char line[512];
                    snprintf(line, sizeof(line), "\n- %s: %s", skill_name, skill_desc);

                    size_t needed = strlen(prompt) + strlen(line) + 1;
                    if (needed > cap) {
                        cap = (cap + needed) * 2;
                        char *tmp = realloc(prompt, cap);
                        if (tmp) prompt = tmp;
                    }
                    strcat(prompt, line);
                    count++;

                    free(name);
                    free(desc);
                    free(fm);
                }
            }
        }
        free(content);
    }
    closedir(dir);

    if (count == 0) {
        free(prompt);
        return NULL;
    }

    return prompt;
}

// Step 8: Loads the skill body (content after closing ---)
static char *load_skill_body(const char *skill_name) {
    char path[512];
    snprintf(path, sizeof(path), ".claude/skills/%s/SKILL.md", skill_name);

    size_t file_len = 0;
    char *content = read_file(path, &file_len);
    if (!content) return NULL;

    char *first_dash = strstr(content, "---");
    if (!first_dash) {
        free(content);
        return NULL;
    }

    char *second_dash = strstr(first_dash + 3, "---");
    if (!second_dash) {
        free(content);
        return NULL;
    }

    char *body = second_dash + 3;
    while (*body == '\r' || *body == '\n' || *body == ' ' || *body == '\t') {
        body++;
    }

    size_t len = strlen(body);
    while (len > 0 && isspace((unsigned char)body[len - 1])) {
        len--;
    }

    char *result = malloc(len + 1);
    if (result) {
        memcpy(result, body, len);
        result[len] = '\0';
    }

    free(content);
    return result;
}

/* ==================================================================== */

static cJSON *call_api(const char *base_url, const char *api_key, cJSON *messages, cJSON *tools) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", "anthropic/claude-haiku-4.5");
    cJSON_AddItemReferenceToObject(req, "messages", messages);
    cJSON_AddItemReferenceToObject(req, "tools", tools);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", base_url);

    char auth_header[1024];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

    CURL *curl = curl_easy_init();
    struct response_buf resp = {NULL, 0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl error: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return NULL;
    }

    cJSON *json = cJSON_Parse(resp.data);
    free(resp.data);
    if (!json) {
        fprintf(stderr, "Failed to parse response JSON\n");
    }
    return json;
}

int main(int argc, char *argv[]) {

    const char *prompt = NULL;
    if (getopt(argc, argv, "p:") == 'p') prompt = optarg;
    if (!prompt) {
        fprintf(stderr, "error: -p flag is required\n");
        return 1;
    }

    const char *api_key = getenv("OPENROUTER_API_KEY");
    const char *base_url = getenv("OPENROUTER_BASE_URL");
    if (!base_url || !*base_url) base_url = "https://openrouter.ai/api/v1";
    if (!api_key || !*api_key) {
        fprintf(stderr, "OPENROUTER_API_KEY is not set\n");
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    cJSON *messages = cJSON_CreateArray();

    // Step 7: Inject system message if skills exist
    char *system_prompt = build_skills_system_prompt();
    if (system_prompt) {
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", system_prompt);
        cJSON_AddItemToArray(messages, sys_msg);
        free(system_prompt);
    }

    // Step 8: Resolve slash command
    const char *final_user_prompt = prompt;
    char *skill_body = NULL;
    char *substituted_body = NULL;

    if (prompt[0] == '/') {
        const char *start = prompt + 1;
        while (*start == ' ') start++;

        const char *end = start;
        while (*end && !isspace((unsigned char)*end)) end++;

        size_t name_len = (size_t)(end - start);
        if (name_len > 0) {
            char skill_name[256];
            if (name_len >= sizeof(skill_name)) name_len = sizeof(skill_name) - 1;
            strncpy(skill_name, start, name_len);
            skill_name[name_len] = '\0';
            
            // everything after skill name represents the arguments
            const char *raw_args = end;
            while(*raw_args == ' ') raw_args++;

            skill_body = load_skill_body(skill_name);
            if (skill_body) {
            	// step 9: substitute $ARGUMENTS, $ARGUMENTS[i], and $i
            	substituted_body = substitute_placeholders(skill_body, raw_args);
                final_user_prompt = substituted_body ? substituted_body : skill_body;
            }
        }
    }

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", final_user_prompt);
    cJSON_AddItemToArray(messages, user_msg);

    cJSON *tools = build_tools();
    int exit_code = 0;

    while (1) {
        cJSON *json = call_api(base_url, api_key, messages, tools);
        if (!json) {
            exit_code = 1;
            break;
        }

        cJSON *choices = cJSON_GetObjectItem(json, "choices");
        if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
            fprintf(stderr, "no choices in response\n");
            cJSON_Delete(json);
            exit_code = 1;
            break;
        }

        cJSON *first = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(first, "message");
        if (!message) {
            fprintf(stderr, "no message in response\n");
            cJSON_Delete(json);
            exit_code = 1;
            break;
        }

        cJSON_AddItemToArray(messages, cJSON_Duplicate(message, 1));

        cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        if (!cJSON_IsArray(tool_calls) || cJSON_GetArraySize(tool_calls) == 0) {
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(message, "content"));
            if (text) {
                printf("%s", text);
            }
            cJSON_Delete(json);
            break;
        }

        cJSON *call = NULL;
        cJSON_ArrayForEach(call, tool_calls) {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(call, "id"));
            cJSON *fn = cJSON_GetObjectItem(call, "function");
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name"));
            const char *args_str = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "arguments"));

            fprintf(stderr, "Executing tool: %s\n", name ? name : "(unknown)");

            char *result = execute_tool(name, args_str);

            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", id ? id : "");
            cJSON_AddStringToObject(tool_msg, "content", result ? result : "");
            cJSON_AddItemToArray(messages, tool_msg);

            free(result);
        }

        cJSON_Delete(json);
    }

	if(substituted_body) free(substituted_body);
    if (skill_body) free(skill_body);
    
    cJSON_Delete(messages);
    cJSON_Delete(tools);
    curl_global_cleanup();

    return exit_code;
    
}
