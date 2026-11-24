#!/usr/bin/env python3
"""Run an already-built public inspector against an explicitly verified model DSO.

No compilation, VM, real SDK fallback or physical hardware access is permitted.
Root supplies a fresh output directory and independently recorded model hash.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time

MODEL_ID = 0x4D4F44454C000001
FORBIDDEN_OPERATIONS = {
    "config_write", "bar_map_NORMAL_RAM_ONLY", "bar_unmap", "mmio_read",
    "mmio_write", "dma_register", "dma_allocate", "buffer_unregister",
    "dma_probe", "irq_create", "irq_set_mask", "irq_drain", "irq_rearm",
    "irq_close", "device_reset", "device_set_dma_address_bits",
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dependencies(path):
    result = subprocess.run(["/usr/bin/otool", "-L", str(path)], check=True,
                            capture_output=True, text=True)
    return [line.strip().split(" (compatibility version", 1)[0]
            for line in result.stdout.splitlines()[1:] if line.startswith("\t")]


def resolve_direct(name, binary):
    if name.startswith("@loader_path/"):
        return (binary.parent / name[len("@loader_path/"):]).resolve()
    if name.startswith("@executable_path/"):
        return (binary.parent / name[len("@executable_path/"):]).resolve()
    if name.startswith("/"):
        return Path(name).resolve()
    raise AssertionError("model SDK must have a direct load command, not @rpath")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--model-library", type=Path, required=True)
    parser.add_argument("--model-sha256", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    binary, model = args.binary.resolve(), args.model_library.resolve()
    assert binary.is_file() and model.is_file()
    assert sha256(model) == args.model_sha256.lower(), "model DSO hash mismatch"
    linked = dependencies(binary)
    sdk_links = [name for name in linked if "libextgpu" in Path(name).name]
    assert len(sdk_links) == 1, "expected exactly one explicit SDK dependency"
    assert resolve_direct(sdk_links[0], binary) == model, "binary does not bind supplied model DSO"
    model_dependencies = dependencies(model)
    # First otool entry may be the DSO's own install name. No SDK delegation/frameworks.
    for name in model_dependencies:
        assert "IOKit" not in name and "DriverKit" not in name
        if "libextgpu" in Path(name).name:
            assert name.startswith("@rpath/") or resolve_direct(name, binary) == model
    args.output_root.mkdir(mode=0o700, parents=False, exist_ok=False)
    results = {"schema": "openpci.public-sdk-inspector-model.v1",
               "modelOnly": True, "physicalHardwareAccess": False,
               "binary": str(binary), "binarySHA256": sha256(binary),
               "modelLibrary": str(model), "modelSHA256": sha256(model),
               "directModelBinding": sdk_links[0], "sdkImplementationSourcesUsed": False,
               "compiledByThisTest": False, "vmStarted": False, "cases": []}

    def run_case(name, argv, returncode):
        log = args.output_root.resolve() / (name + "-model.jsonl")
        env = {key: value for key, value in os.environ.items() if not key.startswith("DYLD_")}
        env.pop("EXTGPU_MODEL_EARLY_IRQ", None)
        env.pop("EXTGPU_MODEL_FAIL_LIFECYCLE", None)
        env.update(EXTGPU_MODEL_ONLY="1", EXTGPU_MODEL_LOG=str(log))
        # Timeout containment is permissible here only because direct model binding
        # and the independently supplied DSO hash were checked before execution.
        process = subprocess.run([str(binary), *argv], env=env, capture_output=True,
                                 text=True, timeout=20)
        (args.output_root / (name + "-stdout.json")).write_text(process.stdout)
        (args.output_root / (name + "-stderr.txt")).write_text(process.stderr)
        assert process.returncode == returncode, (name, process.returncode, process.stderr)
        report = json.loads(process.stdout)
        events = [json.loads(line) for line in log.read_text().splitlines()]
        assert events and events[0]["operation"] == "MODEL_ONLY_CONSTRUCTOR"
        assert all(event["modelOnly"] and not event["hardwareAccess"] for event in events)
        operations = [event["operation"] for event in events]
        assert not FORBIDDEN_OPERATIONS.intersection(operations), (name, operations)
        assert not any(operation.startswith(("mmio_", "bar_internal_backing_")) for operation in operations)
        results["cases"].append({"name": name, "argv": argv, "returncode": process.returncode,
                                 "result": report["result"], "modelEvents": len(events)})
        return report, events, operations

    def run_failure_hold(stage):
        """Contain only our hash-verified, directly bound, model-only child."""
        name = "model-failed-" + stage + "-hold"
        argv = ["--inspect-id", hex(MODEL_ID)]
        if stage != "close":
            argv.append("--claim-pristine")
        log = args.output_root.resolve() / (name + "-model.jsonl")
        stdout_path = args.output_root / (name + "-stdout.json")
        stderr_path = args.output_root / (name + "-stderr.txt")
        env = {key: value for key, value in os.environ.items() if not key.startswith("DYLD_")}
        env.pop("EXTGPU_MODEL_EARLY_IRQ", None)
        env.update(EXTGPU_MODEL_ONLY="1", EXTGPU_MODEL_LOG=str(log),
                   EXTGPU_MODEL_FAIL_LIFECYCLE=stage)
        observation = {"name": name, "modelOnly": True, "argv": argv,
                       "injectedLifecycleFailure": stage,
                       "physicalRecoveryProven": False, "normalCleanupClaimed": False,
                       "forcedTermination": "owned model-test child SIGKILL only"}
        results["cases"].append(observation)
        process = None
        with stdout_path.open("x") as stdout_file, stderr_path.open("x") as stderr_file:
            try:
                process = subprocess.Popen([str(binary), *argv], env=env,
                                           stdout=stdout_file, stderr=stderr_file)
                observation["pid"] = process.pid
                deadline = time.monotonic() + 10
                report = None
                while time.monotonic() < deadline:
                    text = stdout_path.read_text()
                    if text.endswith("\n"):
                        report = json.loads(text)
                        break
                    assert process.poll() is None, (name, "exited before hold report", process.returncode)
                    time.sleep(0.01)
                assert report is not None, (name, "hold JSON deadline exceeded")
                assert report["pid"] == process.pid, "reported PID differs from owned child"
                assert report["result"] == "recovery_needed_process_held" and report["recovery_hold"]
                assert process.poll() is None, (name, "returned after declaring hold")
                actions = report["actions"]
                assert actions["device_handle_live"] and actions["session_handle_live"]
                assert not actions["close_acknowledged"] and not actions["session_destroyed"]
                events = [json.loads(line) for line in log.read_text().splitlines()]
                assert events and events[0]["operation"] == "MODEL_ONLY_CONSTRUCTOR"
                assert all(event["modelOnly"] and not event["hardwareAccess"] for event in events)
                operations = [event["operation"] for event in events]
                assert not FORBIDDEN_OPERATIONS.intersection(operations), (name, operations)
                assert not any(operation.startswith(("mmio_", "bar_internal_backing_")) for operation in operations)
                failed_operation = "device_" + stage
                fault_operation = failed_operation + ("_FAULT_HANDLE_RETAINED_MODEL_ONLY" if stage == "close"
                                                       else "_FAULT_OWNER_RETAINED_MODEL_ONLY")
                enabled = [event for event in events if event["operation"] == "MODEL_ONLY_LIFECYCLE_FAULT_ENABLED"]
                assert len(enabled) == 1 and enabled[0]["status"] == 0
                assert enabled[0]["a"] == {"claim": 1, "release": 2, "close": 3}[stage]
                failure_events = [event for event in events if event["operation"] == fault_operation]
                assert len(failure_events) == 1 and failure_events[0]["status"] == -12
                assert failure_events[0]["a"] == MODEL_ID and failure_events[0]["c"] == 1
                assert failure_events[0]["b"] == (0 if stage == "close" else 1)
                reported_failures = [entry for entry in report["operations"] if entry["operation"] == failed_operation]
                assert len(reported_failures) == 1 and not reported_failures[0]["accepted"]
                assert reported_failures[0]["status"] == failure_events[0]["status"]
                if stage == "claim":
                    assert actions["claim_attempted"] and not actions["claim_succeeded"]
                    assert not actions["release_attempted"]
                    assert "device_release" not in operations and "device_close" not in operations
                    assert "config_read" not in operations and "bar_get_info" not in operations
                elif stage == "release":
                    assert actions["claim_succeeded"] and actions["release_attempted"]
                    assert not actions["release_acknowledged"] and "device_close" not in operations
                    assert operations.count("device_claim") == 1
                else:
                    assert not actions["claim_attempted"] and not actions["release_attempted"]
                    assert "device_claim" not in operations and "device_release" not in operations
                    assert "config_read" not in operations and "bar_get_info" not in operations
                assert "session_destroy" not in operations
                before_log = log.read_bytes()
                before_stdout = stdout_path.read_bytes()
                process.send_signal(signal.SIGTERM)
                # Successful delivery plus continued life is the observable signal
                # deferral contract; the CLI emits no new JSON from its handler.
                signal_deadline = time.monotonic() + 0.3
                while time.monotonic() < signal_deadline:
                    assert process.poll() is None, (name, "SIGTERM dropped held model client")
                    time.sleep(0.01)
                assert log.read_bytes() == before_log, (name, "SDK cleanup/retry occurred after SIGTERM")
                assert stdout_path.read_bytes() == before_stdout
                observation.update(result=report["result"], modelEvents=len(events),
                                   injectedStatus=failure_events[0]["status"],
                                   heldPIDAliveBeforeSIGTERM=True,
                                   heldPIDAliveAfterSIGTERM=True,
                                   noSDKCleanupOrRetryAfterSIGTERM=True,
                                   modelOwnerRetainedAtFault=bool(failure_events[0]["b"]),
                                   modelHandleRetainedAtFault=bool(failure_events[0]["c"]),
                                   modelLogSHA256BeforeAndAfterSIGTERM=hashlib.sha256(before_log).hexdigest())
            finally:
                if process is not None:
                    # This is deliberately not a physical owner-recovery technique.
                    # Popen is the sole source of the PID; no supplied/reused PID is killed.
                    if process.poll() is None:
                        observation["explicitModelOnlySIGKILLRequested"] = True
                        process.kill()
                    observation["returncode"] = process.wait(timeout=5)
                (args.output_root / (name + "-hold-observation.json")).write_text(
                    json.dumps(observation, indent=2) + "\n")
        assert observation["returncode"] == -signal.SIGKILL, (name, "expected explicit model-only SIGKILL")
        observation["status"] = "PASS_MODEL_FAILURE_HOLD_AND_SIGTERM_DEFERRAL"
        (args.output_root / (name + "-hold-observation.json")).write_text(
            json.dumps(observation, indent=2) + "\n")

    try:
        enumeration, _, operations = run_case("enumeration", [], 0)
        assert enumeration["scope"] == "enumeration_only" and len(enumeration["devices"]) == 1
        assert int(enumeration["devices"][0]["registry_id"], 16) == MODEL_ID
        assert "device_open" not in operations and "device_claim" not in operations
        assert enumeration["actions"]["session_destroyed"] and enumeration["identity"] is None
        for name, exact_id in [("exact-hex", hex(MODEL_ID)), ("exact-decimal", str(MODEL_ID))]:
            report, events, operations = run_case(name, ["--inspect-id", exact_id], 0)
            opens = [event for event in events if event["operation"] == "device_open"]
            assert len(opens) == 1 and opens[0]["a"] == MODEL_ID
            assert "enumerate" not in operations and "device_claim" not in operations
            assert report["identity"]["vendor_id"] == 0x1002 and report["identity"]["device_id"] == 0x687F
            assert report["actions"]["close_acknowledged"] and report["actions"]["session_destroyed"]
        report, events, operations = run_case("pristine-claim", ["--inspect-id", hex(MODEL_ID), "--claim-pristine"], 0)
        assert operations.count("device_open") == operations.count("device_claim") == 1
        assert operations.count("device_release") == operations.count("device_close") == 1
        assert operations.index("device_release") < operations.index("device_close") < operations.index("session_destroy")
        reads = [event for event in events if event["operation"] == "config_read"]
        assert [(event["a"], event["b"]) for event in reads] == [(offset, 4) for offset in range(0, 64, 4)]
        assert len(report["header_dwords"]) == 16
        present = {entry["bar"]: entry["metadata"]["size"] for entry in report["bars"] if entry["status"] == 0}
        assert present == {0: 268435456, 2: 2097152, 5: 524288}
        assert all(report["actions"][key] for key in ["claim_succeeded", "release_acknowledged", "close_acknowledged", "session_destroyed"])
        assert not report["recovery_hold"] and not report["actions"]["device_handle_live"]
        report, events, operations = run_case("wrong-exact-id", ["--inspect-id", hex(MODEL_ID + 1)], 1)
        assert operations.count("device_open") == 1 and "enumerate" not in operations and "device_claim" not in operations
        assert [event["a"] for event in events if event["operation"] == "device_open"] == [MODEL_ID + 1]
        assert report["actions"]["session_destroyed"] and not report["actions"]["device_opened"]
        invalid = [[], ["--claim-pristine"], ["--inspect-id"], ["--inspect-id", "1", "--inspect-id", "2"],
                   ["--inspect-id", "1", "--claim-pristine", "--claim-pristine"], ["--unknown"]]
        invalid = invalid[1:] + [["--inspect-id", value] for value in
                   ["", "0", "0x0", "-1", "+1", " 1", "1 ", "0x", "1x", "18446744073709551616", "0x10000000000000000"]]
        for index, argv in enumerate(invalid):
            report, _, operations = run_case("invalid-" + str(index), argv, 2)
            assert report["scope"] == "arguments_only" and operations == ["MODEL_ONLY_CONSTRUCTOR"]
        for stage in ["claim", "release", "close"]:
            run_failure_hold(stage)
        results["status"] = "PASS_PUBLIC_BINARY_SDK_MODEL_INSPECTION_AND_PRISTINE_LIFECYCLE"
        results["failureRecoveryHoldRuntimeTested"] = True
        results["failureRecoveryHoldScope"] = "isolated model-only claim/release/close failures; no physical recovery proof"
        results["physicalDeviceLifecycleProven"] = False
        print(json.dumps({"status": results["status"], "cases": len(results["cases"])}))
    except BaseException as error:
        results["status"] = "FAIL"
        results["error"] = repr(error)
        raise
    finally:
        (args.output_root / "result.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
