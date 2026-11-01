
/* TODO : completed step 6 of build your own claude code Using C on CodeCrafters */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

// Reads an entire file into a heap buffer. Returns NULL on failure.
// Caller must free the returned buffer.
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

// Writes content to a file, creating it or overwriting it.
// Returns 0 on success, -1 on failure.
static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    int close_result = fclose(f);

    if (written != len || close_result != 0) return -1;
    return 0;
}

// Adds a tool spec with the given name, description and parameters to the tools array.
// `params` is a list of {name, description} pairs; all parameters are strings and required.
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

// Builds the tools array advertised to the model.
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
    
    // for step 6
    const char *bash_params[][2] = {
    	{"command", "The command to run"}, };
    add_tool(tools, "Bash", "Executing a shell command", bash_params, 1);
    
    const char *edit_params[][2] = {
    	{"file_path", "The path of the file to edit"},
    	{"old_str", "The string to be replaced"},
    	{"new_str", "The replacement string"}
    };
    add_tool(tools, "Edit", "Replace old_str with new_str in a file", edit_params, 3);

    return tools;
}

static char *run_bash(const char *cmd){

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
    
    // If the command returned no text output, return an empty string rather than NULL
    return output;
    
}

static char *replace_str(const char *src, const char *old_str, const char *new_str) {
    const char *pos = strstr(src, old_str);
    if (!pos) return NULL;

    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t head_len = (size_t)(pos - src);
    size_t tail_len = strlen(pos + old_len);

    char *result = malloc(head_len + new_len + tail_len + 1);
    if (!result) return NULL;

    memcpy(result, src, head_len);
    memcpy(result + head_len, new_str, new_len);
    memcpy(result + head_len + new_len, pos + old_len, tail_len);
    result[head_len + new_len + tail_len] = '\0';

    return result;
}

// Executes a tool call and returns the result as a heap-allocated string.
// Caller must free the returned string.
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
    
    if(strcmp(name, "Bash") == 0){
    	cJSON *args = cJSON_Parse(args_str);
    	const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(args, "command"));
    	if(!cmd){
    		cJSON_Delete(args);
    		return strdup("error: missing command argument");
    	}
    	
    	char *result = run_bash(cmd);
    	cJSON_Delete(args);
    	return result;
    }
    
    if(strcmp(name, "Edit") == 0){
    	cJSON *args = cJSON_Parse(arg_str);
    	const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(args, "file_path"));
    	const char *old_str = cJSON_GetStringValue(cJSON_GetObjectItem(args, "old_str"));
    	const char *new_str = cJSON_GetStringValue(cJSON_GetObjectItem(args, "new_str"));
    	
    	if(!path || !old_str || !new_str){
    		cJSON_Delete(args);
    		return strdup("error: missing required arguments(file_path, old_str, new_str)");
    	}
    	
    	char *content = read_file(path, NULL);
    	if(!content){
    		cJSON_Delete(args);
    		return strdup("error: could not read file");
    	}
    	
    	char *modified = replace_str(content, old_str, new_str);
    	free(content);
    	
    	if(!modified){
    		cJSON_Delete(args);
    		return strdup("error: old_str not found in file");
    	}
    	
    	int write_rc = write_file(path, modified);
    	free(modified);
    	cJSON_Delete(args);
    	
    	if(write_rc != 0){
    		return strdup("error: could not write file");
    	}
    	
    	return strdup("SUCCESS: replaced old_str with new_str");
    }

    return strdup("error: unknown tool");
}

// Sends the conversation to the model and returns the parsed JSON response.
// Returns NULL on failure. Caller must cJSON_Delete the result.
static cJSON *call_api(const char *base_url, const char *api_key, cJSON *messages, cJSON *tools) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", "anthropic/claude-haiku-4.5");
    // References, so deleting `req` doesn't free the shared arrays.
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

    // Conversation history that persists across loop iterations.
    cJSON *messages = cJSON_CreateArray();
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", prompt);
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

        // Record the assistant's message (copy it, since `json` is freed below).
        cJSON_AddItemToArray(messages, cJSON_Duplicate(message, 1));

        cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        if (!cJSON_IsArray(tool_calls) || cJSON_GetArraySize(tool_calls) == 0) {
            // No tool calls: this is the final answer.
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(message, "content"));
            if (text) {
                printf("%s", text);
            }
            cJSON_Delete(json);
            break;
        }

        // Execute every requested tool call and append the results.
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

    cJSON_Delete(messages);
    cJSON_Delete(tools);
    curl_global_cleanup();
    
    return exit_code;
    
}
