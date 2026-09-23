import json, subprocess, sys, time, threading, os
P, ws, clangd = sys.argv[1], sys.argv[2], sys.argv[3]
p = subprocess.Popen([P + "/bin/mcppls", "--payload", P, "--clangd", clangd], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(os.path.join(os.path.dirname(ws),"fail-stderr.log"),"w"))
msgs = {}; notes = []
def send(m):
    b = json.dumps(m).encode(); p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(b) + b); p.stdin.flush()
def reader():
    while True:
        h = b""
        while not h.endswith(b"\r\n\r\n"):
            c = p.stdout.read(1)
            if not c: return
            h += c
        n = int([l for l in h.decode().split("\r\n") if l.lower().startswith("content-length")][0].split(":")[1])
        m = json.loads(p.stdout.read(n))
        if "id" in m and "method" in m:
            notes.append((round(time.time()-t0,1), "REQ " + m["method"], m.get("params")))
            send({"jsonrpc":"2.0","id":m["id"],"result":None})
        elif "id" in m: msgs[m["id"]] = m
        else: notes.append((round(time.time()-t0,1), m["method"], m.get("params")))
t0 = time.time()
threading.Thread(target=reader, daemon=True).start()
def req(i, method, params, timeout=120):
    send({"jsonrpc":"2.0","id":i,"method":method,"params":params}); t=time.time()
    while i not in msgs and time.time()-t<timeout: time.sleep(0.1)
    return msgs.get(i)
uri = lambda f: "file://" + os.path.join(ws, f)
caps = {"experimental":{"cxxModules":{"status":True}},"window":{"showMessage":{}}}
t1=time.time(); r = req(1, "initialize", {"processId":None,"rootUri":"file://"+ws,"workspaceFolders":[{"uri":"file://"+ws,"name":"ws"}],"capabilities":caps})
print("initialize answered after %.1fs" % (time.time()-t1), bool(r and "result" in r))
send({"jsonrpc":"2.0","method":"initialized","params":{}})
for f in ["hello.cppm","main.cpp"]:
    send({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":uri(f),"languageId":"cpp","version":1,"text":open(os.path.join(ws,f)).read()}}})
time.sleep(20)
d = req(10, "textDocument/definition", {"textDocument":{"uri":uri("main.cpp")},"position":{"line":0,"character":8}})
print("definition on `import hello`:", json.dumps((d or {}).get("result"))[:200])
h = req(11, "textDocument/hover", {"textDocument":{"uri":uri("main.cpp")},"position":{"line":1,"character":45}})
print("hover on answer():", json.dumps((h or {}).get("result"))[:200])
c = req(12, "textDocument/completion", {"textDocument":{"uri":uri("main.cpp")},"position":{"line":0,"character":7}})
res = (c or {}).get("result"); items = res.get("items", res) if isinstance(res, dict) else res
print("completion after `import `:", [i.get("label") for i in (items or [])][:5])
time.sleep(3)
print("--- notifications (excluding logMessage/progress):")
seen = {}
for t, m, pr in notes:
    if m in ("window/logMessage", "$/progress"): continue
    key = m + json.dumps(pr)[:160]
    if key in seen: seen[key] += 1; continue
    seen[key] = 1
    print(t, m, json.dumps(pr, ensure_ascii=False)[:420])
logs = [pr.get("message","") for t, m, pr in notes if m == "window/logMessage"]
print("--- logMessage count:", len(logs)); [print("  ", l[:200]) for l in logs if "clangd" in l.lower()][:8]
send({"jsonrpc":"2.0","id":99,"method":"shutdown"}); send({"jsonrpc":"2.0","method":"exit"})
