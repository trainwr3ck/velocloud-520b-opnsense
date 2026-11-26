"""Unit tests for the OPNsense velo5x0-fan daemon. Run from the repo root on any
host with Python 3 (no FreeBSD needed):
    python3 -m pytest -q extras/fan/tests/
The fake bus models the vc_fan kernel contract: F9 (a duty below 255 is
refused unless the watchdog is armed), a pet while disarmed is EPERM;
disarming first writes and verifies 255, and stays armed with EIO if that
fails)."""

import importlib.machinery
import importlib.util
import errno
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
FAN = os.path.join(HERE, "..")          # extras/fan
KMOD = os.path.join(HERE, "..", "..", "..", "src", "vc_fan", "vc_fan.c")
SRC = os.path.join(FAN, "usr", "local", "sbin", "velo5x0-fan")
loader = importlib.machinery.SourceFileLoader("velo5x0_fan", SRC)
spec = importlib.util.spec_from_loader("velo5x0_fan", loader)
fan = importlib.util.module_from_spec(spec)
loader.exec_module(fan)


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class FakeSysctl:
    """hw.vc_fan.* and dev.cpu.N.temperature as the kernel would serve them."""

    def __init__(self, chip_id=0x1D, duty=0xFF, ncpu=4, temp_c=21.0, board="edge540b"):
        self.chip_id, self.duty, self.ncpu = chip_id, duty, ncpu
        self.temp_c = temp_c
        self.wd = 0
        self.tach = 1000
        self.writes = []
        self.configured = 0
        self.pets = 0
        self.fail_reads = set()
        self.fail_writes = False
        self.stuck_duty = None
        self.events = []
        self.board = "edge540b"
        self.releases = 0
        self.board_fails = False
        # on/off fan model (edge520b): tach follows duty when clock is set
        self.clock = None
        self.spin_delay = 0.7               # measured: first valid count 0.3-0.7 s
        self.tach_on = 975                  # measured full-duty count
        self.dead_fan = False
        self.on_at = None

    def _full_verified(self):
        self.duty = 0xFF
        self.writes.append(0xFF)
        return (self.duty if self.stuck_duty is None else self.stuck_duty) == 0xFF

    def get_int(self, name):
        if name in self.fail_reads:
            raise fan.BusError(errno.EIO, name)
        if name == "hw.ncpu":
            return self.ncpu
        if name.startswith("dev.cpu."):
            if self.temp_c is None:
                raise fan.BusError(errno.ENOENT, name)
            return int(round(self.temp_c * 10)) + 2731
        return {"hw.vc_fan.identified": 1 if self.chip_id == 0x1D else 0,
                "hw.vc_fan.chip_id": self.chip_id,
                "hw.vc_fan.duty": self.duty if self.stuck_duty is None else self.stuck_duty,
                "hw.vc_fan.tach": self._tach(),
                "hw.vc_fan.spinup_cfg": 0x01,
                "hw.vc_fan.watchdog_s": self.wd}[name]

    def set_int(self, name, v):
        if name == "hw.vc_fan.duty":
            if self.fail_writes:
                raise fan.BusError(errno.EIO, name)
            if self.chip_id != 0x1D:
                raise fan.BusError(errno.EPERM, name)
            if v != 0xFF and self.wd == 0:
                raise fan.BusError(errno.EPERM, "F9: watchdog disarmed")
            if self.duty == 0 and v != 0 and self.clock is not None:
                self.on_at = self.clock()
            self.duty = v
            self.writes.append(v)
            self.events.append(("duty", v))
            if self.wd:
                self.pets += 1
        elif name == "hw.vc_fan.watchdog_s":
            if v == 0 and self.wd != 0 and self.chip_id == 0x1D:
                if not self._full_verified():
                    raise fan.BusError(errno.EIO, "disarm: full not verified")
            self.wd = v
            self.events.append(("wd", v))
        elif name == "hw.vc_fan.pet":
            if self.wd == 0:
                raise fan.BusError(errno.EPERM, "pet while disarmed")
            self.pets += 1
        elif name == "hw.vc_fan.configure":
            self.configured += 1
            self.events.append(("configure", 1))
        elif name == "hw.vc_fan.pins_failsafe":
            self.releases += 1
            self.events.append(("release", 1))
        else:
            raise KeyError(name)

    def _tach(self):
        if self.clock is None:
            return self.tach
        if self.dead_fan or self.duty == 0 or self.on_at is None:
            return 0x1FFF
        if self.clock() - self.on_at < self.spin_delay:
            return 0x1FFF
        if self.clock() - self.on_at < self.spin_delay + 1.5:
            return 2500                     # spinning up: valid but slow
        return self.tach_on

    def get_str(self, name):
        if name == "hw.vc_fan.board":
            if self.board_fails:
                raise fan.BusError(errno.EIO, name)
            return self.board
        return "emc2104@0x2f: fd=1d"


def nolog(*a):
    pass


class Log:
    def __init__(self):
        self.lines = []

    def __call__(self, level, msg):
        self.lines.append((level, msg))


def cfg(**kw):
    c = dict(fan.DEFAULTS)
    c.update(kw)
    return c


def make(sc=None, temps=None, drives=None, **kw):
    sc = sc or FakeSysctl()
    bus = fan.FanBus(sc)
    clk = Clock()
    seq = list(temps or [])

    def temp_fn():
        if seq:
            sc.temp_c = seq.pop(0)
        return fan.read_coretemp(sc)
    drive_seq = list(drives) if drives is not None else None

    def drive_fn():
        if drive_seq is None:
            return {}
        return drive_seq.pop(0) if len(drive_seq) > 1 else drive_seq[0]
    log = Log()
    c = fan.Controller(bus, temp_fn, cfg(**kw), log, sleep=clk.sleep, clock=clk,
                       drive_fn=drive_fn if drives is not None else None)
    return c, sc, clk, log


class TestPolicy(unittest.TestCase):
    def test_target_duty_bands(self):
        c = cfg()
        self.assertEqual(fan.target_duty(55, c), 24)
        self.assertEqual(fan.target_duty(60, c), 24)
        self.assertEqual(fan.target_duty(80, c), 255)
        self.assertEqual(fan.target_duty(70, c), round(24 + 231 * 0.5))

    def test_drive_bands_from_spec_x_measure(self):
        c = cfg()
        on = False
        for t, want in ((55.0, 0), (57, 0), (60, 69), (62.5, 69), (65, 69), (66, 106),
                        (67.5, 162), (70, 255), (75, 255)):
            d, on = fan.drive_demand(t, on, c)
            self.assertEqual(d, want, t)
        d, on = fan.drive_demand(57, True, c)
        self.assertEqual((d, on), (69, True))
        d, on = fan.drive_demand(55.0, True, c)
        self.assertEqual((d, on), (0, False))

    def test_validate_rejects_bad(self):
        with self.assertRaises(ValueError):
            fan.validate(cfg(on_c=40))
        c = cfg(drive_min_duty=20)
        fan.validate(c)
        self.assertEqual(c["drive_min_duty"], 25)
        with self.assertRaises(ValueError):
            fan.validate(cfg(kick_s=20))
        with self.assertRaises(ValueError):
            fan.validate(cfg(poll_s=20))

    def test_coretemp_decikelvin_and_errors(self):
        sc = FakeSysctl(temp_c=21.0)
        self.assertAlmostEqual(fan.read_coretemp(sc), 21.0, places=1)
        sc.temp_c = None
        self.assertIsNone(fan.read_coretemp(sc))
        sc = FakeSysctl(temp_c=500)
        self.assertIsNone(fan.read_coretemp(sc))        # implausible = fault

    def test_rpm(self):
        self.assertEqual(fan.rpm_from_count(0x1FFF), 0)
        self.assertEqual(fan.rpm_from_count(1000), 3932)


class TestController(unittest.TestCase):
    def test_start_arms_watchdog_before_low_write(self):
        c, sc, clk, log = make(temps=[21])
        c.start()
        self.assertEqual(sc.wd, fan.WATCHDOG_S)
        self.assertEqual(sc.configured, 1)
        c.step()
        self.assertEqual(sc.duty, 0)                    # cool: off

    def test_kernel_refuses_low_duty_unarmed(self):
        sc = FakeSysctl()
        with self.assertRaises(OSError):
            fan.FanBus(sc).write_duty(10)
        self.assertEqual(sc.duty, 0xFF)

    def test_off_on_hysteresis_and_kick(self):
        c, sc, clk, log = make(temps=[21, 55, 59, 61, 55, 51, 50])
        c.start()
        c.step()
        self.assertEqual(sc.duty, 0)
        c.step(); c.step()
        self.assertEqual(sc.duty, 0)                    # below on_c: stays off
        c.step()
        self.assertIn(255, sc.writes)                   # kick
        self.assertEqual(sc.duty, fan.target_duty(61, c.cfg))
        c.step(); c.step()
        self.assertGreaterEqual(sc.duty, 24)            # 55, 51: still running
        c.step()
        self.assertEqual(sc.duty, 0)                    # 50: off

    def test_never_below_stall_free(self):
        c, sc, clk, log = make(temps=[61] + [x / 2 for x in range(100, 170)])
        c.start()
        for _ in range(71):
            c.step()
        for w in sc.writes:
            self.assertTrue(w == 0 or w >= 24, w)

    def test_sensor_error_rereads_duty(self):
        c, sc, clk, log = make(temps=[65])
        c.start(); c.step()
        sc.temp_c = None
        c.temp_fn = lambda: fan.read_coretemp(sc)
        c.step()
        self.assertEqual(sc.duty, 0xFF)
        sc.duty = 0x40                                  # someone lowered it by hand
        c.step()
        self.assertEqual(sc.duty, 0xFF)

    def test_sensor_error_full(self):
        c, sc, clk, log = make(temps=[21])
        c.start(); c.step()
        self.assertEqual(sc.duty, 0)
        sc.temp_c = None
        c.temp_fn = lambda: fan.read_coretemp(sc)
        c.step()
        self.assertEqual(sc.duty, 0xFF)
        self.assertTrue(c.sensor_error)
        p = sc.pets
        c.step()
        self.assertGreater(sc.pets, p)                  # still petting at full

    def test_every_poll_pets(self):
        c, sc, clk, log = make(temps=[21] * 5)
        c.start()
        for _ in range(5):
            p = sc.pets
            c.step()
            self.assertGreater(sc.pets, p)

    def test_reasserts_duty_after_chip_reset(self):
        c, sc, clk, log = make(temps=[21, 21])
        c.start(); c.step()
        sc.duty = 0xFF                                  # someone wrote full by hand
        clk.sleep(3)                                    # past the spin-up settle window
        c.step()
        self.assertEqual(sc.duty, 0)
        self.assertTrue(any("rewriting" in m for _, m in log.lines))

    def test_stall_kicks_then_latch(self):
        c, sc, clk, log = make(temps=[65] * 40)
        c.start(); c.step()
        sc.tach = 0x1FFF
        for _ in range(30):
            c.step()
        self.assertTrue(c.stall_latched)
        self.assertEqual(sc.duty, 0xFF)
        self.assertTrue(any(l == "crit" for l, _ in log.lines))
        sc.tach = 1000
        c.step()
        self.assertEqual(sc.duty, 0xFF)                 # latched until restart

    def test_no_drive_is_cpu_only(self):
        a, sa, _, _ = make(temps=[21, 61, 70, 55, 50])
        b, sb, _, _ = make(temps=[21, 61, 70, 55, 50], drives=[{}])
        for c in (a, b):
            c.start()
            for _ in range(5):
                c.step()
        self.assertEqual(sa.writes, sb.writes)

    def test_drive_max_rule(self):
        c, sc, clk, log = make(temps=[21] * 3, drives=[{"ada0": 67.5}])
        c.start(); c.step()
        self.assertEqual(sc.duty, 162)

    def test_drive_none_reading_ignored(self):
        c, sc, clk, log = make(temps=[21] * 2, drives=[{"da0": None}])
        c.start(); c.step()
        self.assertEqual(sc.duty, 0)

    def test_min_run_10_minutes(self):
        # spec test: drive 61 C, then 54 C after 30 s -> duty stays 69 until 600 s, then 0
        n = 400
        c, sc, clk, log = make(temps=[21] * n, drives=[{"ada0": 61}] * 15 + [{"ada0": 54}])
        c.start()
        start = None
        seen = []
        for _ in range(n):
            c.step()
            if start is None and sc.duty == 69:
                start = clk()
            seen.append((clk(), sc.duty))
            clk.sleep(2)
        self.assertIsNotNone(start)
        for t, d in seen:
            if start <= t < start + 600 - 1:
                self.assertEqual(d, 69, t)
        self.assertEqual(seen[-1][1], 0)

    def test_min_run_starts_on_drive_on_edge_even_with_cpu_running(self):
        # shipped semantics: the hold starts on the drive-demand-on edge,
        # whatever the CPU does; CPU cools, drive drops to 54: duty holds 69
        temps = [65, 65, 65, 45, 45, 45]
        c, sc, clk, log = make(temps=temps, drives=[{}, {"ada0": 61}, {"ada0": 61},
                                                    {"ada0": 61}, {"ada0": 54}, {"ada0": 54}])
        c.start()
        for _ in range(6):
            c.step()
            clk.sleep(2)
        self.assertEqual(sc.duty, 69)

    def test_min_run_cleared_when_drive_vanishes(self):
        c, sc, clk, log = make(temps=[21] * 4, drives=[{"ada0": 61}, {"ada0": 61}, {}, {}])
        c.start()
        for _ in range(4):
            c.step()
            clk.sleep(2)
        self.assertEqual(sc.duty, 0)
        self.assertIsNone(c.drive_hold_until)

    def test_min_run_zero_disables(self):
        c, sc, clk, log = make(temps=[21] * 4, drives=[{"ada0": 61}, {"ada0": 54}], drive_min_run_s=0)
        c.start()
        for _ in range(4):
            c.step()
            clk.sleep(2)
        self.assertEqual(sc.duty, 0)

    def test_min_run_range(self):
        with self.assertRaises(ValueError):
            fan.validate(cfg(drive_min_run_s=3601))
        with self.assertRaises(ValueError):
            fan.validate(cfg(drive_min_run_s=-0.5))

    def test_cpu_failsafe_wins_over_min_run(self):
        c, sc, clk, log = make(temps=[21, 21], drives=[{"ada0": 61}, {"ada0": 54}])
        c.start(); c.step()
        sc.temp_c = None
        c.temp_fn = lambda: fan.read_coretemp(sc)
        c.step()
        self.assertEqual(sc.duty, 0xFF)


class TestLoopAndFailsafes(unittest.TestCase):
    def loop(self, sc, temps, stop_after, exc_at=None):
        clk = Clock()
        bus = fan.FanBus(sc)
        seq = list(temps)
        stop = {"flag": False}
        n = {"i": 0}

        def temp_fn():
            n["i"] += 1
            if exc_at is not None and n["i"] == exc_at:
                raise RuntimeError("boom")
            if n["i"] >= stop_after:
                stop["flag"] = True
            if seq:
                sc.temp_c = seq.pop(0)
            return fan.read_coretemp(sc)
        rc = fan.run_loop(bus, cfg(), nolog, temp_fn, lambda: {}, stop, sleep=clk.sleep, clock=clk)
        return rc

    def test_stop_writes_full_and_disarms(self):
        sc = FakeSysctl()
        rc = self.loop(sc, [21] * 5, stop_after=3)
        self.assertEqual(rc, fan.EXIT_OK)
        self.assertEqual(sc.duty, 0xFF)
        self.assertEqual(sc.wd, 0)

    def test_exception_writes_full_exit1_watchdog_left_armed(self):
        sc = FakeSysctl()
        rc = self.loop(sc, [21] * 5, stop_after=99, exc_at=3)
        self.assertEqual(rc, fan.EXIT_WRITE_FAILED)
        self.assertEqual(sc.duty, 0xFF)
        self.assertEqual(sc.wd, fan.WATCHDOG_S)         # restart re-arms anyway

    def test_unverified_full_raises_alert(self):
        sc = FakeSysctl()
        sc.stuck_duty = 0x00
        log = Log()
        clk = Clock()
        ok = fan.failsafe_full(fan.FanBus(sc), log, "test", alert_file=None,
                               sleep=clk.sleep, clock=clk)
        self.assertFalse(ok)
        self.assertTrue(any(l == "crit" for l, _ in log.lines))

    def test_precondition_wrong_id_writes_nothing(self):
        sc = FakeSysctl(chip_id=0x24)
        log = Log()
        self.assertIsNone(fan.open_bus(log, sc))
        self.assertEqual(sc.writes, [])

    def test_precondition_no_module(self):
        sc = FakeSysctl()
        sc.fail_reads.add("hw.vc_fan.identified")
        self.assertIsNone(fan.open_bus(nolog, sc))


class TestKernelContract(unittest.TestCase):
    def test_pet_eperm_when_disarmed(self):
        sc = FakeSysctl()
        with self.assertRaises(OSError):
            fan.FanBus(sc).pet()

    def test_disarm_forces_full(self):
        sc = FakeSysctl()
        bus = fan.FanBus(sc)
        bus.arm(30)
        bus.write_duty(0)
        bus.arm(0)
        self.assertEqual(sc.duty, 0xFF)
        self.assertEqual(sc.wd, 0)

    def test_disarm_stays_armed_if_full_not_verified(self):
        sc = FakeSysctl()
        bus = fan.FanBus(sc)
        bus.arm(30)
        bus.write_duty(0)
        sc.stuck_duty = 0x00
        with self.assertRaises(OSError):
            bus.arm(0)
        self.assertEqual(sc.wd, 30)

    def test_h1_race_external_disarm_is_caught(self):
        # daemon at cool idle (duty 0); someone runs a full + disarm by hand.
        # The kernel forces 255; the daemon's next write/pet gets EPERM and
        # it goes to verified full (F2) instead of silently running on.
        sc = FakeSysctl()
        clk = Clock()
        bus = fan.FanBus(sc)
        n = {"i": 0}
        stop = {"flag": False}

        def temp_fn():
            n["i"] += 1
            if n["i"] == 3:
                fan.failsafe_full(bus, nolog, "by hand", None, clk.sleep, clk)
                bus.arm(0)
            if n["i"] > 6:
                stop["flag"] = True
            sc.temp_c = 21.0
            return fan.read_coretemp(sc)
        rc = fan.run_loop(bus, cfg(), nolog, temp_fn, lambda: {}, stop, sleep=clk.sleep, clock=clk)
        self.assertEqual(rc, fan.EXIT_WRITE_FAILED)
        self.assertEqual(sc.duty, 0xFF)

    def test_start_verifies_full_before_arming(self):
        c, sc, clk, log = make(sc=FakeSysctl(duty=0))
        c.start()
        i_full = sc.events.index(("duty", 0xFF))
        i_arm = sc.events.index(("wd", fan.WATCHDOG_S))
        self.assertLess(i_full, i_arm)

    def test_start_failure_idles_instead_of_restart_loop(self):
        sc = FakeSysctl()
        sc.stuck_duty = 0x10
        clk = Clock()
        rc = fan.run_loop(fan.FanBus(sc), cfg(), nolog, lambda: 21.0, lambda: {}, {"flag": False},
                          sleep=clk.sleep, clock=clk)
        self.assertEqual(rc, fan.EXIT_IDLE)
        self.assertEqual(sc.wd, 0)                      # never armed

    def test_idle_returns_on_stop(self):
        stop = {"flag": False}

        def sleep(s):
            stop["flag"] = True
        self.assertEqual(fan.idle_until_term(nolog, "test", stop, sleep=sleep), fan.EXIT_OK)

    def test_tach_zero_is_a_bus_fault(self):
        c, sc, clk, log = make(temps=[65] * 4)
        c.start(); c.step()
        sc.tach = 0
        c.step()
        self.assertTrue(c.bus_fault)
        self.assertTrue(c.stall_latched)
        self.assertEqual(sc.duty, 0xFF)
        self.assertTrue(any(l == "crit" for l, _ in log.lines))

    def test_readback_mismatch_becomes_fault(self):
        c, sc, clk, log = make(temps=[21] * 10)
        c.start(); c.step()
        sc.stuck_duty = 0x00 if c.duty else 0x42       # register reads nonsense
        for _ in range(6):
            clk.sleep(2)
            c.step()
        self.assertTrue(c.bus_fault)
        self.assertTrue(any(l == "crit" for l, _ in log.lines))

    def test_no_false_mismatch_inside_spinup(self):
        c, sc, clk, log = make(temps=[21, 21])
        c.start(); c.step()
        sc.stuck_duty = 0x99                            # spin-up level, just written
        c.written_at = clk()
        c.step()
        self.assertEqual(c.mismatches, 0)
        self.assertFalse(any("rewriting" in m for _, m in log.lines))

    def test_set_refused_while_service_runs(self):
        class A:
            duty, hold, force = 100, 4, False
        sc = FakeSysctl()
        rc = fan.cmd_set(A(), nolog, running=lambda: True, sc=sc)
        self.assertEqual(rc, fan.EXIT_PRECONDITION)
        self.assertEqual(sc.writes, [])

    def test_set_ends_full_and_does_not_disarm(self):
        class A:
            duty, hold, force = 100, 4, False
        sc = FakeSysctl()
        clk = Clock()
        rc = fan.cmd_set(A(), nolog, running=lambda: False, sleep=clk.sleep, clock=clk, sc=sc)
        self.assertEqual(rc, fan.EXIT_OK)
        self.assertEqual(sc.duty, 0xFF)
        self.assertEqual(sc.wd, fan.WATCHDOG_S)         # coordinator: no disarm

    def test_full_does_not_disarm(self):
        sc = FakeSysctl()
        fan.FanBus(sc).arm(30)

        class A:
            why = "t"
        orig = fan.open_bus
        fan.open_bus = lambda log, sc_=None: fan.FanBus(sc)
        try:
            self.assertEqual(fan.cmd_full(A(), nolog), fan.EXIT_OK)
        finally:
            fan.open_bus = orig
        self.assertEqual(sc.wd, 30)

    def test_load_refuses_without_board_modules(self):
        calls = []

        class R:
            def __init__(self, rc):
                self.returncode = rc

        def run(argv):
            calls.append(argv)
            if argv[:3] == ["kldstat", "-q", "-m"]:
                return R(0 if argv[3] == "vc_pcu_gpio" else 1)
            return R(0)

        class A:
            path = "/boot/modules/vc_fan.ko"
        self.assertEqual(fan.cmd_load(A(), nolog, run=run), fan.EXIT_PRECONDITION)
        self.assertFalse(any(a[0] == "kldload" for a in calls))


class TestOnOff(unittest.TestCase):
    """edge520b on/off controller."""

    def mk(self, temps, drives=None, dead=False, **kw):
        sc = FakeSysctl(board="edge520b")
        sc.board = "edge520b"
        clk = Clock()
        sc.clock = clk
        sc.on_at = 0.0                      # fan running at full when the daemon starts
        sc.dead_fan = dead
        bus = fan.FanBus(sc)
        seq = list(temps)

        def temp_fn():
            if seq:
                sc.temp_c = seq.pop(0)
            return fan.read_coretemp(sc)
        dseq = list(drives) if drives is not None else None

        def drive_fn():
            if dseq is None:
                return {}
            return dseq.pop(0) if len(dseq) > 1 else dseq[0]
        log = Log()
        c = fan.Controller(bus, temp_fn, cfg(**kw), log, sleep=clk.sleep, clock=clk,
                           drive_fn=drive_fn if drives is not None else None)
        return c, sc, clk, log

    def run_for(self, c, clk, seconds):
        end = clk() + seconds
        while clk() < end:
            c.step()
            clk.sleep(2)

    def test_auto_mode_by_board(self):
        c, sc, clk, log = self.mk([21])
        self.assertEqual(c.mode, "onoff")
        c540, _, _, _ = make()
        self.assertEqual(c540.mode, "curve")

    def test_start_measures_baseline_and_starts_on(self):
        c, sc, clk, log = self.mk([21] * 5)
        c.start()
        self.assertEqual(c.baseline, 975)
        self.assertTrue(c.off_enabled)
        self.assertEqual(c.state, c.ON)
        self.assertEqual(sc.duty, 0xFF)

    def test_duty_only_0_or_255(self):
        temps = [21] * 400 + [65] * 50 + [40] * 400
        c, sc, clk, log = self.mk(temps)
        c.start()
        self.run_for(c, clk, 1700)
        self.assertTrue(set(sc.writes) <= {0, 0xFF}, set(sc.writes))

    def test_min_run_then_off_when_cool(self):
        c, sc, clk, log = self.mk([21] * 400)
        c.start()
        self.run_for(c, clk, 590)
        self.assertEqual(sc.duty, 0xFF)                 # still inside the 600 s minimum run
        self.run_for(c, clk, 20)
        self.assertEqual(sc.duty, 0)
        self.assertEqual(c.state, c.OFF)

    def test_hysteresis_no_on_between_thresholds(self):
        c, sc, clk, log = self.mk([21] * 310 + [55] * 100)
        c.start()
        self.run_for(c, clk, 620)
        self.assertEqual(sc.duty, 0)
        self.run_for(c, clk, 100)                       # 55 C: above OFF, below ON
        self.assertEqual(sc.duty, 0)

    def test_on_at_threshold_with_verified_restart(self):
        c, sc, clk, log = self.mk([21] * 310 + [60] * 10)
        c.start()
        self.run_for(c, clk, 640)
        self.assertEqual(sc.duty, 0xFF)
        self.assertEqual(c.state, c.ON)
        self.assertIsNotNone(c.last_restart_s)
        self.assertLess(c.last_restart_s, 5.0)
        self.assertTrue(any("restart verified" in m for _, m in log.lines))

    def test_no_off_between_thresholds_while_on(self):
        c, sc, clk, log = self.mk([55] * 500)
        c.start()
        self.run_for(c, clk, 900)                       # 55 C: not cool enough for OFF
        self.assertEqual(sc.duty, 0xFF)

    def test_intended_off_is_not_a_stall(self):
        c, sc, clk, log = self.mk([21] * 500)
        c.start()
        self.run_for(c, clk, 900)
        self.assertEqual(sc.duty, 0)
        self.assertFalse(c.stall_latched)
        self.assertFalse(any(l == "crit" for l, _ in log.lines))

    def test_restart_failure_alert_retry_latch_on(self):
        c, sc, clk, log = self.mk([21] * 310 + [65] * 20)
        c.start()
        self.run_for(c, clk, 620)
        self.assertEqual(sc.duty, 0)
        sc.dead_fan = True
        self.run_for(c, clk, 40)
        self.assertTrue(c.stall_latched)
        self.assertEqual(sc.duty, 0xFF)
        crit = [m for l, m in log.lines if l == "crit"]
        self.assertEqual(len([m for m in crit if "did not restart" in m]), 3)
        self.assertTrue(any("latched ON" in m for m in crit))
        self.run_for(c, clk, 800)                       # latched: never OFF again, even cool
        self.assertEqual(sc.duty, 0xFF)

    def test_stall_while_on_latches(self):
        c, sc, clk, log = self.mk([65] * 30)
        c.start()
        c.step()
        sc.dead_fan = True
        self.run_for(c, clk, 10)
        self.assertTrue(c.stall_latched)
        self.assertEqual(sc.duty, 0xFF)

    def test_sensor_loss_from_off_goes_full_and_checks_restart(self):
        c, sc, clk, log = self.mk([21] * 400)
        c.start()
        self.run_for(c, clk, 620)
        self.assertEqual(sc.duty, 0)
        sc.temp_c = None
        c.temp_fn = lambda: fan.read_coretemp(sc)
        c.step()
        self.assertEqual(sc.duty, 0xFF)
        self.assertEqual(c.state, c.ON)
        self.assertFalse(c.stall_latched)               # fan restarted fine

    def test_drive_demand_turns_on(self):
        c, sc, clk, log = self.mk([21] * 500, drives=[{}] * 320 + [{"ada0": 61}])
        c.start()
        self.run_for(c, clk, 620)
        self.assertEqual(sc.duty, 0)
        self.run_for(c, clk, 30)
        self.assertEqual(sc.duty, 0xFF)

    def test_no_baseline_means_latched_on(self):
        c, sc, clk, log = self.mk([21] * 500, dead=True)
        c.start()
        self.assertFalse(c.off_enabled)
        self.assertTrue(c.stall_latched)
        self.run_for(c, clk, 900)
        self.assertEqual(sc.duty, 0xFF)

    def test_watchdog_kept_alive_in_off(self):
        c, sc, clk, log = self.mk([21] * 500)
        c.start()
        self.run_for(c, clk, 620)
        p = sc.pets
        self.run_for(c, clk, 20)
        self.assertGreater(sc.pets, p)

    def test_curve_mode_unchanged_on_540(self):
        a, sa, _, _ = make(temps=[21, 61, 70, 55, 50])
        a.start()
        for _ in range(5):
            a.step()
        self.assertEqual(a.mode, "curve")
        self.assertIn(fan.target_duty(70, a.cfg), sa.writes)


class TestOnOffSafety(unittest.TestCase):
    def mk520(self, **kw):
        sc = FakeSysctl(board="edge520b")
        sc.board = "edge520b"
        clk = Clock()
        sc.clock = clk
        sc.on_at = 0.0
        for k, v in kw.items():
            setattr(sc, k, v)
        return sc, clk

    def test_full_verified_before_configure(self):
        sc, clk = self.mk520(duty=0)
        c = fan.Controller(fan.FanBus(sc), lambda: 21.0, cfg(), nolog, sleep=clk.sleep, clock=clk)
        c.start()
        ev = sc.events
        self.assertLess(ev.index(("duty", 0xFF)), ev.index(("configure", 1)))

    def test_board_read_error_is_failsafe(self):
        sc, clk = self.mk520(board_fails=True)
        rc = fan.run_loop(fan.FanBus(sc), cfg(), nolog, lambda: 21.0, lambda: {}, {"flag": False},
                          sleep=clk.sleep, clock=clk)
        self.assertEqual(rc, fan.EXIT_IDLE)
        self.assertEqual(sc.duty, 0xFF)
        self.assertGreaterEqual(sc.releases, 1)
        self.assertEqual(sc.configured, 0)

    def test_curve_refused_on_520(self):
        sc, clk = self.mk520()
        rc = fan.run_loop(fan.FanBus(sc), cfg(mode="curve"), nolog, lambda: 21.0, lambda: {},
                          {"flag": False}, sleep=clk.sleep, clock=clk)
        self.assertEqual(rc, fan.EXIT_IDLE)
        self.assertEqual(sc.configured, 0)
        self.assertGreaterEqual(sc.releases, 1)

    def test_start_failure_releases(self):
        sc, clk = self.mk520(stuck_duty=0x10)
        rc = fan.run_loop(fan.FanBus(sc), cfg(), nolog, lambda: 21.0, lambda: {}, {"flag": False},
                          sleep=clk.sleep, clock=clk)
        self.assertEqual(rc, fan.EXIT_IDLE)
        self.assertGreaterEqual(sc.releases, 1)
        self.assertEqual(sc.configured, 0)                      # never drove bit 7

    def test_latches_release(self):
        t = TestOnOff()
        c, sc, clk, log = t.mk([21] * 310 + [65] * 20)
        c.start()
        t.run_for(c, clk, 620)
        sc.dead_fan = True
        r0 = sc.releases
        t.run_for(c, clk, 40)
        self.assertTrue(c.stall_latched)
        self.assertGreater(sc.releases, r0)

    def test_bus_fault_releases(self):
        c, sc, clk, log = TestOnOff().mk([65] * 10)
        c.start(); c.step()
        sc.tach = 0
        sc.clock = None                                         # raw tach value 0
        r0 = sc.releases
        c.step()
        self.assertTrue(c.bus_fault)
        self.assertGreater(sc.releases, r0)

    def test_sensor_error_restart_failure_releases(self):
        c, sc, clk, log = TestOnOff().mk([21] * 400)
        c.start()
        TestOnOff().run_for(c, clk, 620)
        self.assertEqual(sc.duty, 0)                    # fan OFF
        sc.dead_fan = True                              # ...and it will not restart
        sc.temp_c = None                                # ...and the CPU sensor fails
        c.temp_fn = lambda: fan.read_coretemp(sc)
        r0 = sc.releases
        c.step()
        self.assertEqual(sc.duty, 0xFF)
        self.assertTrue(c.stall_latched)
        self.assertGreater(sc.releases, r0)             # bit 7 released: full by hardware

    def test_clean_stop_releases(self):
        sc, clk = self.mk520()
        stop = {"flag": False}
        n = {"i": 0}

        def temp_fn():
            n["i"] += 1
            if n["i"] > 2:
                stop["flag"] = True
            return 21.0
        rc = fan.run_loop(fan.FanBus(sc), cfg(), nolog, temp_fn, lambda: {}, stop,
                          sleep=clk.sleep, clock=clk)
        self.assertEqual(rc, fan.EXIT_OK)
        self.assertEqual(sc.duty, 0xFF)
        self.assertEqual(sc.events[-1][0] in ("wd", "release"), True)
        self.assertGreaterEqual(sc.releases, 1)


class TestShell(unittest.TestCase):
    def test_sh_syntax(self):
        root = os.path.join(FAN, "usr", "local", "etc")
        for p in ("rc.d/velo5x0_fand", "rc.syshook.d/early/06-velo-fan"):
            r = subprocess.run(["sh", "-n", os.path.join(root, p)])
            self.assertEqual(r.returncode, 0, p)

    def test_enable_line(self):
        with open(os.path.join(FAN, "etc", "rc.conf.d", "velo5x0_fand")) as f:
            self.assertEqual(f.read().strip(), 'velo5x0_fand_enable="NO"')

    def test_shipped_conf_matches_defaults(self):
        c = fan.load_config(os.path.join(FAN, "usr", "local", "etc", "velo5x0-fan", "fan.conf"))
        for k, v in fan.DEFAULTS.items():
            if k in fan.STR_KEYS:
                self.assertEqual(c[k], v, k)
            else:
                self.assertEqual(float(c[k]), float(v), k)
        fan.validate(c)

    def test_kmod_static(self):
        with open(KMOD) as f:
            src = f.read()
        self.assertNotIn("MODULE_DEPEND(vc_fan, vc_board", src)
        self.assertNotIn('#include "vc_board.h"', src)
        full = src[src.index("vcf_full_q(const char *why, int quiet)"):]
        full = full[:full.index("\n}\n")]
        self.assertNotIn("pca1c", full)     # the duty fail-safe itself never drives a pin
        apply_calls = [l for l in src.splitlines() if "pca1c_apply_locked()" in l and "static" not in l
                       and not l.startswith("pca1c")]
        self.assertEqual(len(apply_calls), 1)                           # only in configure
        self.assertIn("pca1c_release_locked", src)
        self.assertIn("pca1c_check_levels", src)
        self.assertIn("lvl=%02x", src)

    def test_kmod_520b_bit7_only_and_failsafe_release(self):
        with open(KMOD) as f:
            src = f.read()
        self.assertIn("return (vcf_board == VCF_BOARD_520B ? 0x80 : PCA_FAN_BITS);", src)
        self.assertNotIn("allow_520_pins", src)
        for where in ("vcf_wd_task_fn", "vcf_shutdown(void *arg", "vcf_unload(void)",
                      "vcf_load(void)"):
            body = src[src.index(where):]
            body = body[:body.index("\n}\n")]
            self.assertIn("vcf_failsafe_pins_locked(", body, where)
        rel = src[src.index("vcf_failsafe_pins_locked(const char *why)"):]
        rel = rel[:rel.index("\n}\n")]
        self.assertIn("pca1c_rmw(PCA_CFG, 0x80, 0)", rel)
        self.assertNotIn("vcf_pins_saved &&", rel)

        for where in ("vcf_wd_task_fn", "vcf_shutdown(void *arg"):
            body = src[src.index(where):]
            self.assertLess(body.index("vcf_failsafe_pins_locked("), body.index("vcf_full_locked("))

    def test_kmod_no_eagain_from_sysctl(self):
        # the sysctl layer retries a handler returning EAGAIN (kernel livelock)
        with open(KMOD) as f:
            src = f.read()
        self.assertNotIn("return (EAGAIN)", src)

    def test_kmod_refused_load_guard(self):
        with open(KMOD) as f:
            src = f.read()
        unload = src[src.index("vcf_unload(void)"):]
        guard = unload.index("if (!vcf_inited)")
        self.assertLess(guard, unload.index("sx_xlock"))
        load = src[src.index("vcf_load(void)"):src.index("vcf_unload(void)")]
        self.assertLess(load.index("sx_init"), load.index("vcf_inited = 1"))

    def test_kmod_never_addresses_0x18_or_pol(self):
        with open(KMOD) as f:
            src = f.read()
        self.assertIn("if (addr == PCA_RESETS_ADDR)\n\t\treturn (EPERM);", src)
        self.assertNotIn("PCA_POL, ", src.split("sysctl_dump")[0].split("pca1c_rmw(PCA_")[-1])
        self.assertNotIn("SUS", "".join(l for l in src.splitlines() if "pin_set" in l))


if __name__ == "__main__":
    unittest.main()
