#include "mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>

// LOW LEVEL I/O & JSON-RPC TRANSPORT HELPERS

// reads a whole file into a heap-allocated null-terminated buffer
static char *read_entire_file(const char *path){
	
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	
	if(fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long len = ftell(f);
	if(len < 0) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	
	char *buf = malloc((size_t)len + 1);
	if(!buf) {
		fclose(f);
		return NULL;
	}
	
	size_t read_bytes = fread(buf, 1, (size_t)len, f);
	fclose(f);
	buf[read_bytes] = '\0';
	
	return buf;
}

// Writes a single line-delimited JSON payload to the child process's stdin.
static int mcp_send_json(MCP_Server *server, cJSON *json) {

    char *rendered = cJSON_PrintUnformatted(json);
    if (!rendered) return -1;

    size_t len = strlen(rendered);
    ssize_t written = write(server->in_fd, rendered, len);
    free(rendered);

    if (written < 0 || (size_t)written != len) return -1;

    // MCP stdio messages are strictly newline-delimited
    if (write(server->in_fd, "\n", 1) != 1) return -1;

    return 0;
}

// Reads one line from the child process's stdout and parses it as JSON.
static cJSON *mcp_read_json(MCP_Server *server) {
    size_t cap = 2048;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    char ch;
    while (1) {
        ssize_t r = read(server->out_fd, &ch, 1);
        if (r <= 0) {
            // EOF or error
            free(buf);
            return NULL;
        }

        if (ch == '\n') {
            // End of message frame
            break;
        }

        if (ch == '\r') {
            // Ignore carriage return
            continue;
        }

        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) {
                free(buf);
                return NULL;
            }
            buf = tmp;
        }
        buf[len++] = ch;
    }

    buf[len] = '\0';
    if (len == 0) {
        free(buf);
        return NULL;
    }

    cJSON *parsed = cJSON_Parse(buf);
    free(buf);
    return parsed;
}

// Sends a JSON-RPC request and synchronously awaits the matching response
static cJSON *mcp_call_rpc(MCP_Server *server, const char *method, cJSON *params) {

    int req_id = server->next_request_id++;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", method);
    if (params) {
        cJSON_AddItemToObject(req, "params", params);
    } else {
        cJSON_AddObjectToObject(req, "params");
    }

    if (mcp_send_json(server, req) != 0) {
        cJSON_Delete(req);
        return NULL;
    }
    cJSON_Delete(req);

    // Read messages until we get a response object with our matching ID
    while (1) {
        cJSON *resp = mcp_read_json(server);
        if (!resp) return NULL;

        cJSON *id_item = cJSON_GetObjectItem(resp, "id");
        if (id_item && cJSON_IsNumber(id_item) && id_item->valueint == req_id) {
            return resp;
        }

        // Drop unrelated notifications / logs from child
        cJSON_Delete(resp);
    }
}

// Sends a JSON-RPC notification (no ID, no response expected)
static int mcp_send_notification(MCP_Server *server, const char *method, cJSON *params) {

    cJSON *notif = cJSON_CreateObject();
    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", method);
    
    if (params) {
        cJSON_AddItemToObject(notif, "params", params);
    } else {
        cJSON_AddObjectToObject(notif, "params");
    }

    int rc = mcp_send_json(server, notif);
    cJSON_Delete(notif);
    return rc;
}

//SUBPROCESSES LIFECYCLE

// Spawns a child process with connected stdin/stdout pipes.
static int mcp_spawn_process(MCP_Server *server, const char *command,
                             char *const argv[], char *const envp[]) {
    int p_to_c[2]; // Parent writes -> Child reads (stdin)
    int c_to_p[2]; // Child writes -> Parent reads (stdout)

    if (pipe(p_to_c) < 0) return -1;
    if (pipe(c_to_p) < 0) {
        close(p_to_c[0]);
        close(p_to_c[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(p_to_c[0]); close(p_to_c[1]);
        close(c_to_p[0]); close(c_to_p[1]);
        return -1;
    }

    if (pid == 0) {
        // --- Child Process ---
        close(p_to_c[1]); // Close unused write-end
        close(c_to_p[0]); // Close unused read-end

        // Connect stdin
        if (dup2(p_to_c[0], STDIN_FILENO) < 0) {
            _exit(127);
        }
        close(p_to_c[0]);

        // Connect stdout
        if (dup2(c_to_p[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(c_to_p[1]);

        // Apply custom environment variables if specified
        if (envp) {
            for (int i = 0; envp[i] != NULL; i++) {
                putenv(envp[i]);
            }
        }

        execvp(command, argv);
        _exit(127); // exec failed
    }

    // --- Parent Process ---
    close(p_to_c[0]); // Close unused child read-end
    close(c_to_p[1]); // Close unused child write-end

    server->pid = pid;
    server->in_fd = p_to_c[1];
    server->out_fd = c_to_p[0];
    server->next_request_id = 1;

    return 0;
}

//HANDSHAKE & DISCOVERY

static int mcp_server_initialize(MCP_Server *server) {

    // 1. Send "initialize" request
    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");

    cJSON *caps = cJSON_AddObjectToObject(init_params, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");

    cJSON *client_info = cJSON_AddObjectToObject(init_params, "clientInfo");
    cJSON_AddStringToObject(client_info, "name", "my-claude-code");
    cJSON_AddStringToObject(client_info, "version", "1.0.0");

    cJSON *resp = mcp_call_rpc(server, "initialize", init_params);
    if (!resp) return -1;

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (!result) {
        cJSON_Delete(resp);
        return -1;
    }
    cJSON_Delete(resp);

    // 2. Send "notifications/initialized" notification
    mcp_send_notification(server, "notifications/initialized", NULL);

    // 3. Request "tools/list"
    cJSON *list_resp = mcp_call_rpc(server, "tools/list", NULL);
    if (!list_resp) return -1;

    cJSON *list_result = cJSON_GetObjectItem(list_resp, "result");
    cJSON *tools_array = list_result ? cJSON_GetObjectItem(list_result, "tools") : NULL;

    server->tools = NULL;
    server->tool_count = 0;

    if (tools_array && cJSON_IsArray(tools_array)) {
        int count = cJSON_GetArraySize(tools_array);
        server->tools = calloc(count, sizeof(MCP_Tool));

        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(tools_array, i);
            cJSON *name = cJSON_GetObjectItem(item, "name");
            cJSON *desc = cJSON_GetObjectItem(item, "description");
            cJSON *schema = cJSON_GetObjectItem(item, "inputSchema");

            if (name && cJSON_IsString(name)) {
                server->tools[server->tool_count].name = strdup(name->valuestring);
                server->tools[server->tool_count].description =
                    (desc && cJSON_IsString(desc)) ? strdup(desc->valuestring) : strdup("");
                server->tools[server->tool_count].input_schema =
                    schema ? cJSON_Duplicate(schema, 1) : cJSON_CreateObject();
                server->tool_count++;
            }
        }
    }

    cJSON_Delete(list_resp);
    return 0;
}

//Public API Implementation


MCP_Manager *mcp_manager_init(const char *config_path) {

    char *content = read_entire_file(config_path);
    if (!content) return NULL;

    cJSON *config = cJSON_Parse(content);
    free(content);
    if (!config) return NULL;

    cJSON *servers_map = cJSON_GetObjectItem(config, "mcpServers");
    if (!servers_map || !cJSON_IsObject(servers_map)) {
        cJSON_Delete(config);
        return NULL;
    }

    int server_count = cJSON_GetArraySize(servers_map);
    if (server_count == 0) {
        cJSON_Delete(config);
        return NULL;
    }

    MCP_Manager *mgr = calloc(1, sizeof(MCP_Manager));
    mgr->servers = calloc(server_count, sizeof(MCP_Server));

    cJSON *server_entry = NULL;
    cJSON_ArrayForEach(server_entry, servers_map) {
        const char *server_name = server_entry->string;
        cJSON *cmd_item = cJSON_GetObjectItem(server_entry, "command");
        if (!cmd_item || !cJSON_IsString(cmd_item)) continue;

        const char *command = cmd_item->valuestring;
        cJSON *args_item = cJSON_GetObjectItem(server_entry, "args");
        cJSON *env_item = cJSON_GetObjectItem(server_entry, "env");

        // Build argv array
        int argc = 1;
        if (args_item && cJSON_IsArray(args_item)) {
            argc += cJSON_GetArraySize(args_item);
        }
        char **argv = malloc((argc + 1) * sizeof(char *));
        argv[0] = strdup(command);

        int arg_idx = 1;
        if (args_item && cJSON_IsArray(args_item)) {
            cJSON *arg = NULL;
            cJSON_ArrayForEach(arg, args_item) {
                if (cJSON_IsString(arg)) {
                    argv[arg_idx++] = strdup(arg->valuestring);
                }
            }
        }
        argv[arg_idx] = NULL;

        // Build envp array
        char **envp = NULL;
        if (env_item && cJSON_IsObject(env_item)) {
        
            int envc = cJSON_GetArraySize(env_item);
            envp = malloc((envc + 1) * sizeof(char *));
            int env_idx = 0;
            cJSON *var = NULL;
            cJSON_ArrayForEach(var, env_item) {
            
                if (cJSON_IsString(var)) {
                    size_t len = strlen(var->string) + strlen(var->valuestring) + 2;
                    envp[env_idx] = malloc(len);
                    snprintf(envp[env_idx], len, "%s=%s", var->string, var->valuestring);
                    env_idx++;
                }
            }
            envp[env_idx] = NULL;
        }

        MCP_Server *s = &mgr->servers[mgr->count];
        s->name = strdup(server_name);

        if (mcp_spawn_process(s, command, argv, envp) == 0) {
            if (mcp_server_initialize(s) == 0) {
                mgr->count++;
            } else {
                close(s->in_fd);
                close(s->out_fd);
                kill(s->pid, SIGKILL);
                waitpid(s->pid, NULL, 0);
                free(s->name);
            }
        } else {
            free(s->name);
        }

        for (int i = 0; i < arg_idx; i++) free(argv[i]);
        free(argv);

        if (envp) {
            for (int i = 0; envp[i] != NULL; i++) free(envp[i]);
            free(envp);
        }
    }

    cJSON_Delete(config);

    if (mgr->count == 0) {
        free(mgr->servers);
        free(mgr);
        return NULL;
    }

    return mgr;
}

void mcp_manager_register_tools(MCP_Manager *mgr, cJSON *tools_array) {

    if (!mgr || !tools_array) return;

    for (int s = 0; s < mgr->count; s++) {
        MCP_Server *server = &mgr->servers[s];
        for (int t = 0; t < server->tool_count; t++) {
            MCP_Tool *tool = &server->tools[t];

            cJSON *fn_tool = cJSON_CreateObject();
            cJSON_AddStringToObject(fn_tool, "type", "function");

            cJSON *fn = cJSON_AddObjectToObject(fn_tool, "function");
            cJSON_AddStringToObject(fn, "name", tool->name);
            cJSON_AddStringToObject(fn, "description", tool->description);

            // Directly attach the JSON schema as the parameters field
            cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(tool->input_schema, 1));

            cJSON_AddItemToArray(tools_array, fn_tool);
        }
    }
}

int mcp_manager_has_tool(MCP_Manager *mgr, const char *tool_name) {

    if (!mgr || !tool_name) return 0;

    for (int s = 0; s < mgr->count; s++) {
        MCP_Server *server = &mgr->servers[s];
        for (int t = 0; t < server->tool_count; t++) {
            if (strcmp(server->tools[t].name, tool_name) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

char *mcp_manager_call_tool(MCP_Manager *mgr, const char *tool_name, const char *args_json_str) {

    if (!mgr || !tool_name) return strdup("error: invalid tool call");

    MCP_Server *target_server = NULL;
    for (int s = 0; s < mgr->count; s++) {
        MCP_Server *server = &mgr->servers[s];
        for (int t = 0; t < server->tool_count; t++) {
            if (strcmp(server->tools[t].name, tool_name) == 0) {
                target_server = server;
                break;
            }
        }
        if (target_server) break;
    }

    if (!target_server) {
        return strdup("error: tool not found on any active MCP server");
    }

    cJSON *call_params = cJSON_CreateObject();
    cJSON_AddStringToObject(call_params, "name", tool_name);

    cJSON *parsed_args = args_json_str ? cJSON_Parse(args_json_str) : NULL;
    if (parsed_args) {
        cJSON_AddItemToObject(call_params, "arguments", parsed_args);
    } else {
        cJSON_AddObjectToObject(call_params, "arguments");
    }

    cJSON *resp = mcp_call_rpc(target_server, "tools/call", call_params);
    if (!resp) {
        return strdup("error: failed to receive response from MCP server");
    }

    cJSON *error_item = cJSON_GetObjectItem(resp, "error");
    if (error_item) {
    
        cJSON *msg = cJSON_GetObjectItem(error_item, "message");
        const char *err_text = msg && cJSON_IsString(msg) ? msg->valuestring : "unknown RPC error";
        size_t needed = strlen(err_text) + 32;
        char *out = malloc(needed);
        if (out) snprintf(out, needed, "MCP Error: %s", err_text);
        cJSON_Delete(resp);
        return out ? out : strdup("error: internal error");
    }

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *content_array = result ? cJSON_GetObjectItem(result, "content") : NULL;

    if (!content_array || !cJSON_IsArray(content_array) || cJSON_GetArraySize(content_array) == 0) {
        cJSON_Delete(resp);
        return strdup("");
    }

    // Accumulate all text elements returned in content
    size_t total_len = 0;
    int items = cJSON_GetArraySize(content_array);
    for (int i = 0; i < items; i++) {
        cJSON *item = cJSON_GetArrayItem(content_array, i);
        cJSON *text = cJSON_GetObjectItem(item, "text");
        if (text && cJSON_IsString(text)) {
            total_len += strlen(text->valuestring) + 1;
        }
    }

    char *output = malloc(total_len + 1);
    if (!output) {
        cJSON_Delete(resp);
        return strdup("error: out of memory");
    }
    output[0] = '\0';

    size_t offset = 0;
    for (int i = 0; i < items; i++) {
        cJSON *item = cJSON_GetArrayItem(content_array, i);
        cJSON *text = cJSON_GetObjectItem(item, "text");
        
        if (text && cJSON_IsString(text)) {
        
            size_t tlen = strlen(text->valuestring);
            memcpy(output + offset, text->valuestring, tlen);
            offset += tlen;
            output[offset++] = '\n';
        }
    }
    if (offset > 0 && output[offset - 1] == '\n') {
        output[offset - 1] = '\0';
    } else {
        output[offset] = '\0';
    }

    cJSON_Delete(resp);
    return output;
}

void mcp_manager_cleanup(MCP_Manager *mgr) {

    if (!mgr) return;

    for (int s = 0; s < mgr->count; s++) {
        MCP_Server *server = &mgr->servers[s];

        close(server->in_fd);
        close(server->out_fd);

        kill(server->pid, SIGTERM);
        waitpid(server->pid, NULL, 0);

        for (int t = 0; t < server->tool_count; t++) {
            free(server->tools[t].name);
            free(server->tools[t].description);
            cJSON_Delete(server->tools[t].input_schema);
        }
        free(server->tools);
        free(server->name);
    }

    free(mgr->servers);
    free(mgr);
}
