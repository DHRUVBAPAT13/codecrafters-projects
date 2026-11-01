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

    // Helper: Read file contents and return as allocated string (does NOT print to stdout)
    char *read_file_content(const char *file_path) {
        FILE *fd = fopen(file_path, "rb");
        if (!fd) {
            fprintf(stderr, "failed to open file: %s\n", file_path);
            return strdup("");
        }

        fseek(fd, 0, SEEK_END);
        long file_size = ftell(fd);
        fseek(fd, 0, SEEK_SET);

        char *content = malloc(file_size + 1);
        if (!content) {
            fprintf(stderr, "failed to allocate memory for file content\n");
            fclose(fd);
            return strdup("");
        }

        size_t read_bytes = fread(content, 1, file_size, fd);
        content[read_bytes] = '\0';
        fclose(fd);
        return content;
    }

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

    // Helper: Make HTTP POST request to OpenRouter/LLM
    cJSON *send_chat_completion(const char *url, const char *auth_header, cJSON *req_body) {
        char *body = cJSON_PrintUnformatted(req_body);

        CURL *curl = curl_easy_init();
        if (!curl) {
            free(body);
            return NULL;
        }

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

        char url[512];
        snprintf(url, sizeof(url), "%s/chat/completions", base_url);

        char auth_header[512];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

        curl_global_init(CURL_GLOBAL_DEFAULT);

        // Build the persistent request object
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "model", "anthropic/claude-haiku-4.5");
        cJSON *messages = cJSON_AddArrayToObject(req, "messages");

        // Define "Read" tool
        cJSON *tools = cJSON_AddArrayToObject(req, "tools");
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON *function = cJSON_CreateObject();
        cJSON_AddItemToObject(tool, "function", function);
        cJSON_AddStringToObject(function, "name", "Read");
        cJSON_AddStringToObject(function, "description", "Read and return the contents of a file");
        cJSON *parameters = cJSON_CreateObject();
        cJSON_AddItemToObject(function, "parameters", parameters);
        cJSON_AddStringToObject(parameters, "type", "object");
        cJSON *properties = cJSON_CreateObject();
        cJSON_AddItemToObject(parameters, "properties", properties);
        cJSON *file_path = cJSON_CreateObject();
        cJSON_AddItemToObject(properties, "file_path", file_path);
        cJSON_AddStringToObject(file_path, "type", "string");
        cJSON_AddStringToObject(file_path, "description", "The path to the file to read");
        cJSON *required = cJSON_CreateArray();
        cJSON_AddItemToArray(required, cJSON_CreateString("file_path"));
        cJSON_AddItemToObject(parameters, "required", required);
        cJSON_AddItemToArray(tools, tool);

        // Initial user prompt
        cJSON *initial_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(initial_msg, "role", "user");
        cJSON_AddStringToObject(initial_msg, "content", prompt);
        cJSON_AddItemToArray(messages, initial_msg);

        // === AGENT LOOP ===
        while (1) {
            cJSON *json = send_chat_completion(url, auth_header, req);
            if (!json) {
                fprintf(stderr, "Failed to get API response\n");
                break;
            }

            cJSON *choices = cJSON_GetObjectItem(json, "choices");
            if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
                fprintf(stderr, "No choices in response\n");
                cJSON_Delete(json);
                break;
            }

            cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
            cJSON *assistant_msg = cJSON_GetObjectItem(first_choice, "message");

            // 1. Add assistant response message to conversation history
            cJSON_AddItemToArray(messages, cJSON_Duplicate(assistant_msg, 1));

            cJSON *tool_calls = cJSON_GetObjectItem(assistant_msg, "tool_calls");

            // 2. If no tool calls requested, print final content and exit
            if (!tool_calls || !cJSON_IsArray(tool_calls) || cJSON_GetArraySize(tool_calls) == 0) {
                cJSON *content = cJSON_GetObjectItem(assistant_msg, "content");
                if (cJSON_IsString(content) && content->valuestring) {
                    printf("%s", content->valuestring);
                }
                cJSON_Delete(json);
                break;
            }

            // 3. Process each tool call
            int num_tool_calls = cJSON_GetArraySize(tool_calls);
            for (int i = 0; i < num_tool_calls; i++) {
                cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
                cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
                cJSON *tc_func = cJSON_GetObjectItem(tc, "function");
                cJSON *tc_func_name = cJSON_GetObjectItem(tc_func, "name");
                cJSON *tc_args = cJSON_GetObjectItem(tc_func, "arguments");

                char *result_str = NULL;
                if (tc_func_name && strcmp(tc_func_name->valuestring, "Read") == 0) {
                    cJSON *args_json = cJSON_Parse(cJSON_GetStringValue(tc_args));
                    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(args_json, "file_path"));
                    result_str = read_file_content(path);
                    cJSON_Delete(args_json);
                } else {
                    result_str = strdup("");
                }

                // 4. Append tool result message to conversation history
                cJSON *tool_resp_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_resp_msg, "role", "tool");
                cJSON_AddStringToObject(tool_resp_msg, "tool_call_id", tc_id ? tc_id->valuestring : "");
                cJSON_AddStringToObject(tool_resp_msg, "content", result_str ? result_str : "");
                cJSON_AddItemToArray(messages, tool_resp_msg);

                free(result_str);
            }

            cJSON_Delete(json);
        }

        cJSON_Delete(req);
        curl_global_cleanup();
        return 0;
    }
