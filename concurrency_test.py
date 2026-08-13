"""真并发测试（第 2 轮）：验证部分拒绝的路径。

5 个线程 barrier 同步后同时发起请求，打印每个请求的
success / exit_code / error_message / stdout，便于区分「同意」与「拒绝」。

运行: python concurrency_test.py
期间请到 QQ 对部分请求点「❌ Deny」、其余点「✅ Approve」。
"""
import sys
import os
import time
import threading
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "client", "mcp"))
from tcp_client import send_command

HOST = "127.0.0.1"
PORT = 12380
N = 5

t_start = time.time()
barrier = threading.Barrier(N)


def worker(i):
    cmd = f"echo BATCH3_MIXED_{i} && whoami"
    ts = time.time() - t_start
    print(f"[{i}] sending at +{ts:.3f}s  cmd={cmd}", flush=True)
    r = send_command(HOST, PORT, cmd, timeout_seconds=300)
    tr = time.time() - t_start
    if isinstance(r, dict):
        ok = r.get("success")
        out = (r.get("stdout_output", "") or "").strip().replace("\n", " ")
        err = (r.get("error_message", "") or "").strip()
        print(f"[{i}] DONE +{tr:.3f}s  success={ok} exit={r.get('exit_code')} "
              f"err={err!r} out={out!r}", flush=True)
    else:
        print(f"[{i}] DONE +{tr:.3f}s  -> {r!r}", flush=True)
    return i


def run(i):
    barrier.wait()
    worker(i)


if __name__ == "__main__":
    print(f"launching {N} concurrent requests at t=0", flush=True)
    with ThreadPoolExecutor(max_workers=N) as ex:
        futs = [ex.submit(run, i) for i in range(N)]
        for f in futs:
            f.result()
    print("ALL DONE", flush=True)
