#ifndef MCP_H
#define MCP_H

#include <cjson/cJSON.h>
#include <sys/types.h>

// represents an individual discovered MCP tool
typedef struct{
	char *name;
	char *description;
	cJSON *input_schema;
} MCP_Tool;

// represents an active connection to an MCP server process
typedef struct{
	char *name;
	pid_t pid;
	int in_fd;		// parent writes to child stdin
	int out_fd;		// child reads from the parent stdout
	int next_request_id;
	MCP_Tool *tools;
	int tool_count;
} MCP_Server;

// holds all managed servers
typedef struct{
	MCP_Server *servers;
	int count;
} MCP_Manager;
	
/* 
	Initializes the MCP manager by reading a config file (e.g., .mcp.json)
	Spawns each configured server, executes the JSON-RPC initialize handshake, and retrieves the
	available tools via tools/list
	Returns a pointer to the manager, or NULL on complete failure 
*/
 MCP_Manager *mcp_manager_init(const char *config_path);
 
/*
	appends all discovered MCP tools to the agent's LLM tools array
	converts each tool's schema into OpenAI/OpenRouter function-calling format 
*/
void mcp_manager_register_tools(MCP_Manager *mgr, cJSON *tools_array);

/*
	checks whether a given tool name belongs to any managed MCP server
	returns 1 if handled by MCP, 0 otherwise 
*/
int mcp_manager_has_tool(MCP_Manager *mgr, const char *tool_name);

/*
	executes a tool on the responsible MCP server via JSON-RPC tools/call
	returns a heap-allocated string with the tool output
	caller must free the returned string
*/
char *mcp_manager_call_tool(MCP_Manager *mgr, const char *tool_name, const char *args_json_str);

/*
	sends SIGTERM, closes file descriptors, waits on child processes and frees all allocated memory 
*/ 
void mcp_manager_cleanup(MCP_Manager *mgr);

#endif // MCP_H
