#ifndef SESSION_H
#define SESSION_H

#include <cjson/cJSON.h>
#include <stdio.h>
#include <time.h>

#define SESSION_DIR ".claude/sessions"
#define SESSION_ID_MAX 64
#define SESSION_PATH_MAX 512

// encapsulates an active persistent session
typedef struct{
	char id[SESSION_ID_MAX];			// unique session identifier (timestamp or hash)
	char file_path[SESSION_PATH_MAX];		// full path to .cluade/sessions/<id>.jsonl
	FILE *file;					// append-only handle for active logging
	int is_resumed;					// flag: 1 if resumed from existing, 0 if new 
	time_t created_at;				// session initialization timestamp
} Session;

/*
	initializes or resolves a session based on user CLI intent:
		- if session_id is provided, attempts to open/resume that specific session
		- if continue_latest is true (non-zero), resolves the most recently .jsonl
		- if neither is specified (both NULL/0), generates a new session file
		
	ensures the .claude/sessions directory exists
	returns a heap-allocated Session pointer, or NULL on failure
*/
Session *session_open(const char *session_id, int continue_latest);

/*
	reads historical message entries from the session's JSONL file and populates the messages cJSON
	array (used before sending the payload to the LLM)
	Skips metadata lines (e.g. session_init)
	
	- Returns the number of restored message objects added to the array
*/
int session_load_history(Session *s, cJSON *message);

/*
	appends a new message object to the session's JSONL file
	accepts any standard message object (role: user, assistant, or tool)
	serializes the object unformatted followed by a newline '\n'
	flushes immediately to prevent data loss on unexpected exits
*/
int session_append_message(Session *s, const cJSON *message);

/*
	closes the open file handle and frees the allocated Session struct
*/
void session_close(Session *s);

/*
	helper to locate the mostly recently modified .jsonl file inside .claude/sessions/
	copies the resolved session ID (without the .jsonl extension) into out_id
	returns 1 if a session was found, 0 if no sessions exists or on error
*/
int session_find_latest_id(char *out_id, size_t max_len);

/*
	appends an assistant message containing tool_calls to the session JSONL
*/
void session_log_assistant_tool_calls(Session *sess, cJSON *tool_calls_array, const char *optional_content);

/*
	appends a tool execution result (role: tool) to the session JSONL
*/
void session_log_tool_result(Session *sess, const char *tool_call_id, const char *tool_name, const char *result_content);

/*
	scans .claude/sessions/ and prints a formatted summary table
	displaying session id, last modified timestamp, and message turn count
*/
void session_list_all(void);

/*
	returns 1 if .claude/sessions/<session_id>.jsonl exists, 0 otherwise
*/
int session_exists(const char *session_id);

#endif // SESSION_H
