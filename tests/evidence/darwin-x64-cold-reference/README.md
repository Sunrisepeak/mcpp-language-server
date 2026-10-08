# Native Intel macOS stock cold references timeout

Product63567e3 run37770044349/job113292253715 failed the unchanged15s editor stress budget on action2, references in src/greet/detail.cppm importing std. Official clangd23.1.0 ea7d852a received the request at11:46:45.308 and returned real locations at11:47:02.791,17.482s later. References execution itself was at most1ms. Detail prerequisites took16.83s; its AST/index/diagnostics finished11:47:00.520, when the request was already15.212s old. An additional2.271s before task execution lacks a native trace and cannot be attributed to a particular lock or CPU queue.

Background index priority was already enabled, -j3. Background std indexing occurred concurrently, but the log does not quantify its contention. The stress summary counts its timed-out sample as answered:39 requests met their budgets and one returned after timing out. The production forwarding path actually forwarded this request; the caller timeout did not prove a lost request.

[Exact artifact, source, log digest and lines](timeline.json). This is retained negative evidence, not a runtime fix. Neither editor budgets nor readiness assertions were changed. The final maintained native engine still needs the same editor qualification.
