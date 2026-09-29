import json
import os
import pathlib
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


ROOT = pathlib.Path(__file__).resolve().parents[1]
BASE = "http://127.0.0.1:8080/watch/test-code"


def request(path, method="GET", headers=None):
    req = urllib.request.Request(BASE + path, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=8) as response:
            return response.status, response.read(4096)
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def main():
    with tempfile.TemporaryDirectory() as config:
        env = dict(os.environ)
        env.update(
            CAMWEAVE_TEST_SOURCE="1",
            CAMWEAVE_AUTOSTART_TEST="1",
            CAMWEAVE_TEST_CODE="test-code",
            XDG_CONFIG_HOME=config,
        )
        process = subprocess.Popen([str(ROOT / "build/camweave-camera")], cwd=ROOT, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"Camera exited early: {process.stdout.read()}")
                try:
                    status, payload = request("/status")
                    if status == 200:
                        break
                except urllib.error.URLError:
                    pass
                time.sleep(0.2)
            else:
                raise RuntimeError("Camera did not start listening")

            assert json.loads(payload)["quality"] == 720
            assert request("/")[0] == 200
            try:
                urllib.request.urlopen("http://127.0.0.1:8080/watch/wrong/status", timeout=5)
            except urllib.error.HTTPError as error:
                assert error.code == 404
            else:
                raise AssertionError("Wrong code was accepted")
            assert request("/settings?quality=1080&fps=15", method="POST")[0] == 403
            assert request("/settings?quality=1080&fps=15", method="POST", headers={"X-Camera-Control": "1"})[0] == 202
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                state = json.loads(request("/status")[1])
                if state["quality"] == 1080 and state["fps"] == 15 and state["frames"] > 0:
                    break
                time.sleep(0.2)
            else:
                raise AssertionError("Settings or synthetic video were not applied")
            with urllib.request.urlopen(BASE + "/stream", timeout=8) as response:
                assert response.headers.get_content_type() == "multipart/x-mixed-replace"
                assert b"--frame" in response.read(80)
            print("CamWeave Linux native capture and browser protocol checks passed")
        except BaseException:
            print(f"Camera process exit code: {process.poll()}")
            raise
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            if process.stdout:
                output = process.stdout.read()
                if output:
                    print("Camera output:\n" + output[-5000:])


if __name__ == "__main__":
    main()
