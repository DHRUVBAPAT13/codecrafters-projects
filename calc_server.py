#!/usr/bin/env python3
import sys
import json

def send_response(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()

def handle_request(req):
    req_id = req.get("id")
    method = req.get("method")
    params = req.get("params", {})

    if method == "initialize":
        send_response({
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {
                    "tools": {}
                },
                "serverInfo": {
                    "name": "mock-calc-server",
                    "version": "1.0.0"
                }
            }
        })
    elif method == "notifications/initialized":
        # Notification: no response required
        pass
    elif method == "tools/list":
        send_response({
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "tools": [
                    {
                        "name": "calculate",
                        "description": "Performs basic math arithmetic on two numbers",
                        "inputSchema": {
                            "type": "object",
                            "properties": {
                                "operation": {
                                    "type": "string",
                                    "description": "Operation: add, subtract, multiply, divide"
                                },
                                "a": { "type": "number", "description": "First number" },
                                "b": { "type": "number", "description": "Second number" }
                            },
                            "required": ["operation", "a", "b"]
                        }
                    }
                ]
            }
        })
    elif method == "tools/call":
        tool_name = params.get("name")
        args = params.get("arguments", {})

        if tool_name == "calculate":
            op = args.get("operation")
            a = float(args.get("a", 0))
            b = float(args.get("b", 0))

            if op == "add":
                res = a + b
            elif op == "subtract":
                res = a - b
            elif op == "multiply":
                res = a * b
            elif op == "divide":
                res = a / b if b != 0 else "Error: Division by zero"
            else:
                res = f"Error: Unknown operation {op}"

            send_response({
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "content": [
                        {
                            "type": "text",
                            "text": f"Result: {res}"
                        }
                    ]
                }
            })
        else:
            send_response({
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {
                    "code": -32601,
                    "message": f"Method {tool_name} not found"
                }
            })

def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            handle_request(req)
        except Exception as e:
            sys.stderr.write(f"Server error: {e}\n")
            sys.stderr.flush()

if __name__ == "__main__":
    main()
