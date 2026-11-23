<?php
// SPDX-License-Identifier: GPL-2.0-or-later

##|+PRIV
##|*IDENT=page-status-xgs-npu
##|*NAME=Status: XGS NPU
##|*DESCR=Allow access to the XGS NPU status and testing page.
##|*MATCH=xgs_octep_status.php*
##|-PRIV

require_once("guiconfig.inc");

const XGS_LOGDIR = "/var/tmp/xgs-tests";
const XGS_PCAPDIR = "/home/pcaps";
const XGS_PATHFILE = "/usr/local/etc/xgs_octep.path";
const XGS_LOADER = "/boot/loader.conf.local";
const XGS_FRAME_BYTES = 1500;

function xgs_run($cmd, &$rc = null) {
	$out = array();
	exec($cmd . " 2>&1", $out, $rc);
	return trim(implode("\n", $out));
}

function xgs_repo() {
	$p = is_readable(XGS_PATHFILE) ? trim((string)file_get_contents(XGS_PATHFILE)) : "";
	if ($p === "" || !is_dir($p . "/driver")) {
		$p = "/home/xgs-octeon-pfsense-ce";
	}
	return $p;
}

function xgs_kv($raw) {
	$kv = array();
	foreach (preg_split('/\s+/', $raw, -1, PREG_SPLIT_NO_EMPTY) as $word) {
		if (strpos($word, "=") !== false) {
			list($k, $v) = explode("=", $word, 2);
			$kv[$k] = $v;
		}
	}
	return $kv;
}

function xgs_accel() {
	$out = array();
	foreach (explode("\n", xgs_run("/sbin/sysctl -n dev.xgs_octep.0.accel")) as $line) {
		if (preg_match('/^(\d+)=(\d+)$/', trim($line), $m)) {
			$out[(int)$m[1]] = (int)$m[2];
		}
	}
	return $out;
}

function xgs_ifpkts($ifn) {
	$awk = 'NR==1 {for (i = 1; i <= NF; i++) h[$i] = i} NR==2 {print $(h["Ipkts"]), $(h["Opkts"])}';
	$line = xgs_run("/usr/bin/netstat -I " . escapeshellarg($ifn) . " -n | /usr/bin/awk " . escapeshellarg($awk));
	$f = preg_split('/\s+/', $line);
	return array("in" => (int)($f[0] ?? 0), "out" => (int)($f[1] ?? 0));
}

function xgs_pcap_pids() {
	$pids = array();
	foreach (array("octep0", "octep1") as $i) {
		$pid = (int)@file_get_contents("/var/run/xgs-pcap-" . $i . ".pid");
		if ($pid > 0 && in_array(xgs_run("/bin/ps -p " . $pid . " -o comm="), array("tcpdump", "timeout"), true)) {
			$pids[$i] = $pid;
		}
	}
	return $pids;
}

function xgs_pcap_limit($v) {
	return max(5, min(600, (int)$v > 0 ? (int)$v : 120));
}

function xgs_pcap_start($nm, $secs = 120) {
	if (count(xgs_pcap_pids()) > 0) {
		return false;
	}
	@mkdir(XGS_PCAPDIR, 0755, true);
	@mkdir(XGS_LOGDIR, 0755, true);
	foreach (array("octep0", "octep1") as $i) {
		@unlink("/var/run/xgs-pcap-" . $i . ".pid");
		xgs_run("/usr/sbin/daemon -f -p /var/run/xgs-pcap-" . $i . ".pid -o " . XGS_LOGDIR . "/pcap-" . $i . ".log /usr/bin/timeout -k 5 -s INT " . xgs_pcap_limit($secs) . " /usr/sbin/tcpdump -n -p --immediate-mode -U -s 96 -c 1000000 -i " . $i . " -w " . escapeshellarg(XGS_PCAPDIR . "/" . $nm . "-" . $i . ".pcap") . " tcp port 5201");
	}
	usleep(500000);
	$GLOBALS["xgs_pcap_fail"] = "";
	foreach (array("octep0", "octep1") as $i) {
		if (!isset(xgs_pcap_pids()[$i])) {
			$GLOBALS["xgs_pcap_fail"] .= " " . $i . ": not running; " . trim((string)@file_get_contents(XGS_LOGDIR . "/pcap-" . $i . ".log"));
		}
	}
	xgs_run("/bin/ls -t " . XGS_PCAPDIR . "/*.pcap 2>/dev/null | /usr/bin/tail -n +5 | /usr/bin/xargs -r /bin/rm -f");
	return true;
}

function xgs_pcap_stop() {
	$pids = xgs_pcap_pids();
	foreach ($pids as $pid) {
		xgs_run("/bin/kill " . (int)$pid);
	}
	return count($pids);
}

function xgs_sample() {
	$a = xgs_accel();
	$g = function ($i) use ($a) {
		return $a[$i] ?? 0;
	};
	list(, , $kv) = xgs_status_kv();
	return array(
		"t" => microtime(true),
		"drv_stalls" => (int)($kv["stalls"] ?? 0),
		"drv_stall_max" => (int)($kv["stall_max_s"] ?? 0),
		"offloaded" => $g(2),
		"to_host" => $g(3),
		"non_accel" => $g(40),
		"not_active" => $g(59),
		"octep0" => xgs_ifpkts("octep0"),
		"octep1" => xgs_ifpkts("octep1"),
	);
}

function xgs_gate($kv) {
	return isset($kv["offload"], $kv["fw_cfg"]) && $kv["offload"] === "1" && hexdec($kv["fw_cfg"]) === 3;
}

function xgs_status_kv() {
	$raw = xgs_run("/sbin/sysctl -n dev.xgs_octep.0.status", $rc);
	return array($raw, $rc, xgs_kv($raw));
}

$loaded = xgs_run("/sbin/kldstat -q -n xgs_octep && echo yes") === "yes";

if (isset($_GET["ajax"])) {
	header("Content-Type: application/json");
	echo json_encode($loaded ? xgs_sample() : array("error" => "driver not loaded"));
	exit;
}

$present = xgs_run("/usr/sbin/pciconf -l | /usr/bin/grep -c -E 'chip=0xa300177d|vendor=0x177d[[:space:]]+device=0xa300'") !== "0";
$repo = xgs_repo();
$raw = "";
$st_rc = null;
$all = array();
if ($loaded) {
	list($raw, $st_rc, $all) = xgs_status_kv();
}
$gate_on = xgs_gate($all);

$msg = "";
$msgclass = "info";

if ($_SERVER["REQUEST_METHOD"] === "POST") {
	$action = $_POST["action"] ?? "";
	@mkdir(XGS_PCAPDIR, 0755, true);
	$env = "PCAP_DIR=" . XGS_PCAPDIR . " ";
	$pcapname = trim($_POST["pcapname"] ?? "");
	if ($pcapname !== "") {
		if (preg_match('/^[A-Za-z0-9._-]{1,64}$/', $pcapname)) {
			$env .= "PCAP_NAME=" . escapeshellarg($pcapname) . " ";
		} else {
			$msg = gettext("Rejected: pcap name may only contain letters, digits, . _ -");
			$msgclass = "danger";
			$action = "";
		}
	}
	$pcaplimit = xgs_pcap_limit($_POST["pcaplimit"] ?? 120);
	$autopcap = "";
	if ($pcapname !== "" && $action !== "") {
		if (in_array($action, array("cutin", "live_on", "iperf_client", "iperf_server"), true)) {
			if (xgs_pcap_start($pcapname, $pcaplimit)) {
				$autopcap = " " . sprintf(gettext("pcap armed: %s/%s-octepN.pcap (stops after %d s)."), XGS_PCAPDIR, $pcapname, $pcaplimit) . $xgs_pcap_fail;
			}
		} elseif (in_array($action, array("iperf_stop", "live_off"), true)) {
			if (xgs_pcap_stop() > 0) {
				$autopcap = " " . gettext("pcap stopped.");
			}
		}
	}
	if (in_array($action, array("boot", "sysctl", "cutin", "plain"), true)) {
		if ($action !== "plain" && !$gate_on) {
			$msg = gettext("Refused: offload gate is not ON (no test without verified offload). Switch it ON first, or run plain.");
			$msgclass = "danger";
		} else {
			xgs_run("/usr/sbin/daemon -f /usr/bin/env " . $env . "/bin/sh " . escapeshellarg($repo . "/driver/offload-test.sh") . " " . $action);
			$msg = sprintf(gettext("Started offload-test.sh %s. Log: %s/<stamp>-%s.log"), $action, XGS_LOGDIR, $action);
		}
	} elseif ($action === "pcap_start") {
		$nm = $pcapname !== "" ? $pcapname : date("Ymd-His");
		if (xgs_pcap_start($nm, $pcaplimit)) {
			$msg = sprintf(gettext("pcap started: %s/%s-octep0.pcap and -octep1.pcap (tcp port 5201, 96 B snaplen, stops after %d s)."), XGS_PCAPDIR, $nm, $pcaplimit) . $xgs_pcap_fail;
		} else {
			$msg = gettext("A pcap is already running. Stop it first.");
			$msgclass = "danger";
		}
	} elseif ($action === "pcap_stop") {
		$msg = xgs_pcap_stop() > 0 ? gettext("pcap stopped.") : gettext("No pcap is running.");
	} elseif ($action === "live_on" || $action === "live_off") {
		$msg = xgs_run("/bin/sh " . escapeshellarg($repo . "/driver/offload-ctl.sh") . " " . ($action === "live_on" ? "on" : "off"));
	} elseif ($action === "boot_on" || $action === "boot_off") {
		$lines = is_readable(XGS_LOADER) ? file(XGS_LOADER, FILE_IGNORE_NEW_LINES) : array();
		$lines = array_values(array_filter($lines, function ($l) {
			return !preg_match('/^\s*hw\.xgs_octep\.offload\s*=/', $l);
		}));
		$v = $action === "boot_on" ? "1" : "0";
		$lines[] = 'hw.xgs_octep.offload="' . $v . '"';
		file_put_contents(XGS_LOADER, implode("\n", $lines) . "\n");
		$msg = sprintf(gettext("Boot setting written: hw.xgs_octep.offload=%s (applies next boot)."), $v);
	} elseif ($action === "iperf_stop") {
		$n = (int)xgs_run("/bin/pgrep -x iperf3 | /usr/bin/wc -l");
		if ($n > 0) {
			xgs_run("/bin/pkill -x iperf3");
			$msg = sprintf(gettext("Stopped %d iperf3 process(es)."), $n);
		} else {
			$msg = gettext("No iperf3 is running.");
		}
	} elseif ($action === "iperf_server" || $action === "iperf_client") {
		if (!is_executable("/usr/local/bin/iperf3")) {
			$msg = gettext("iperf3 is not installed on this box (pkg install iperf3).");
			$msgclass = "danger";
		} else {
			@mkdir(XGS_LOGDIR, 0755, true);
			$log = XGS_LOGDIR . "/" . date("Ymd-His") . "-iperf.log";
			$d = "/usr/sbin/daemon -f -o " . escapeshellarg($log) . " /usr/local/bin/iperf3";
			if ($action === "iperf_server") {
				xgs_run($d . " -s -1");
				$msg = sprintf(gettext("iperf3 server started (one test, then exits). Log: %s"), $log);
			} else {
				$from = $_POST["iperf_from"] ?? "custom";
				$bind = trim($_POST["iperf_bind"] ?? "");
				if ($from === "octep0" || $from === "octep1") {
					$bind = xgs_run("/sbin/ifconfig " . $from . " inet | /usr/bin/awk '/inet /{print $2; exit}'");
					if ($bind === "") {
						$msg = sprintf(gettext("%s has no IPv4 address."), $from);
						$msgclass = "danger";
					}
				}
				$srv = trim($_POST["iperf_srv"] ?? "");
				$secs = (int)($_POST["iperf_secs"] ?? 30);
				if ($msgclass === "danger") {
					// interface had no address, message already set
				} elseif (!filter_var($srv, FILTER_VALIDATE_IP) || ($bind !== "" && !filter_var($bind, FILTER_VALIDATE_IP)) || $secs < 1 || $secs > 300) {
					$msg = gettext("Rejected: server and bind must be IP addresses; seconds 1-300.");
					$msgclass = "danger";
				} else {
					$d .= ($bind !== "" ? " -B " . escapeshellarg($bind) : "") . " -c " . escapeshellarg($srv) . " -t " . $secs;
					xgs_run($d);
					$msg = sprintf(gettext("iperf3 client started from %s (%d s). Log: %s"), $bind !== "" ? $bind : "default", $secs, $log);
				}
			}
		}
	}
	if ($loaded) {
		list($raw, $st_rc, $all) = xgs_status_kv();
		$gate_on = xgs_gate($all);
	}
}

$facilities = array();
$kv = array();
foreach ($all as $k => $v) {
	if (in_array($k, array("control", "mgmt_netdev", "nw_agent", "rpc", "giu"))) {
		$facilities[$k] = $v;
	} else {
		$kv[$k] = $v;
	}
}
$ready = isset($kv["ready"]) && $kv["ready"] === "1";
$nwa = isset($kv["nwa"]) && $kv["nwa"] === "1";

$issues = array();
if (!$loaded) {
	$issues[] = gettext("driver not loaded");
} elseif ($raw === "") {
	$issues[] = sprintf(gettext("status unreadable (rc=%s)"), var_export($st_rc, true));
} else {
	$ports = (int)($kv["ports"] ?? 0);
	if (!$ready) { $issues[] = gettext("NPU not ready"); }
	if (!$nwa) { $issues[] = gettext("network agent not found"); }
	if (($kv["rpc_ready"] ?? "0") !== "1") { $issues[] = gettext("rpc not ready"); }
	if ($ports === 0 || (int)($kv["ports_up"] ?? 0) !== $ports) { $issues[] = sprintf(gettext("ports up %s of %s"), $kv["ports_up"] ?? "?", $ports); }
	if ((int)($kv["ifaces"] ?? 0) === 0) { $issues[] = gettext("octep interfaces not created"); }
	if (($kv["last_err"] ?? "0") !== "0") { $issues[] = sprintf(gettext("last error %s"), $kv["last_err"]); }
	if ((int)($kv["rx_err"] ?? 0) > 0 || (int)($kv["tx_drop"] ?? 0) > 0) { $issues[] = sprintf(gettext("rx_err %s tx_drop %s"), $kv["rx_err"] ?? 0, $kv["tx_drop"] ?? 0); }
}
$healthy = count($issues) === 0;

if ($ready && $nwa) {
	$state = gettext("Up: the NPU answered and its ports were discovered");
	$class = "success";
} elseif ($loaded) {
	$state = gettext("Driver loaded; waiting for the NPU (it can take minutes after boot)");
	$class = "warning";
} else {
	$state = gettext("Driver not loaded");
	$class = "danger";
}

$loader_val = "unset";
if (is_readable(XGS_LOADER) && preg_match('/^\s*hw\.xgs_octep\.offload\s*=\s*"?(\d)"?/m', (string)file_get_contents(XGS_LOADER), $m)) {
	$loader_val = $m[1];
}
$logs = array_slice(array_reverse(glob(XGS_LOGDIR . "/*.log") ?: array()), 0, 6);
$viewtext = "";
$viewlog = basename($_GET["log"] ?? "");
if ($viewlog !== "" && in_array(XGS_LOGDIR . "/" . $viewlog, $logs, true)) {
	$viewtext = xgs_run("/usr/bin/tail -n 80 " . escapeshellarg(XGS_LOGDIR . "/" . $viewlog));
}
$iperf_rate = "";
foreach ($logs as $l) {
	if (strpos($l, "-iperf.log") !== false) {
		$iperf_rate = xgs_run("/usr/bin/grep 'Mbits/sec' " . escapeshellarg($l) . " | /usr/bin/tail -n 1");
		break;
	}
}

$pgtitle = array(gettext("Status"), gettext("XGS NPU"));
include("head.inc");
?>

<?php if ($msg !== ""): ?>
<div class="alert alert-<?=$msgclass?>"><pre style="margin:0;background:none;border:0"><?=htmlspecialchars($msg . ($autopcap ?? ""))?></pre></div>
<?php endif; ?>

<div class="alert alert-<?=$gate_on ? "success" : "danger"?>" style="font-size:18px;font-weight:bold">
	<?=gettext("OFFLOAD")?>: <span class="label label-<?=$gate_on ? "success" : "danger"?>" style="font-size:18px"><?=$gate_on ? "ON" : "OFF"?></span>
	<span style="font-size:13px;font-weight:normal">&nbsp;<?=gettext("live gate")?> fw_cfg=<?=htmlspecialchars($kv["fw_cfg"] ?? "-")?> &nbsp;|&nbsp; <?=gettext("at boot")?>: <?=$loader_val === "1" ? "ON" : ($loader_val === "0" ? "OFF" : "unset")?></span>
</div>

<div class="alert alert-<?=$healthy ? "success" : "danger"?>"><b>OCTEP</b>: <?php if ($healthy): ?><?=sprintf(gettext("HEALTHY - %s of %s ports up, %s interfaces, no errors"), $kv["ports_up"], $kv["ports"], $kv["ifaces"])?><?php else: ?><?=gettext("NOT HEALTHY")?> - <?=htmlspecialchars(implode("; ", $issues))?><?php endif; ?></div>

<div class="alert alert-<?=$class?>"><?=htmlspecialchars($state)?></div>

<div class="panel panel-default">
	<div class="panel-heading"><h2 class="panel-title"><?=gettext("Coprocessor")?></h2></div>
	<div class="panel-body table-responsive">
		<table class="table table-striped table-hover">
			<tbody>
				<tr><td><?=gettext("PCI endpoint 177d:a300")?></td><td><?=$present ? gettext("present") : gettext("not found")?></td></tr>
				<tr><td><?=gettext("Driver xgs_octep")?></td><td><?=$loaded ? gettext("loaded") : gettext("not loaded")?></td></tr>
				<tr><td><?=gettext("NPU ready")?></td><td><?=$ready ? gettext("yes") : gettext("no")?></td></tr>
				<tr><td><?=gettext("Barmap version")?></td><td><?=htmlspecialchars($kv["barmap_version"] ?? "-")?></td></tr>
				<tr><td><?=gettext("Network agent")?></td><td><?=$nwa ? gettext("discovered") : gettext("not yet")?></td></tr>
				<tr><td><?=gettext("Ports reported / up")?></td><td><?=htmlspecialchars(($kv["ports"] ?? "-") . " / " . ($kv["ports_up"] ?? "-"))?></td></tr>
				<tr><td><?=gettext("Offload gate, live")?></td><td><?=$gate_on ? "1 (ON)" : "0 (OFF)"?> &nbsp; fw_cfg=<?=htmlspecialchars($kv["fw_cfg"] ?? "-")?></td></tr>
				<tr><td><?=gettext("Offload at boot (loader.conf.local)")?></td><td><?=htmlspecialchars($loader_val)?></td></tr>
				<tr><td><?=gettext("Last error")?></td><td><?=htmlspecialchars($kv["last_err"] ?? "-")?></td></tr>
			</tbody>
		</table>
		<form method="post" action="xgs_octep_status.php">
			<button class="btn btn-success btn-sm" name="action" value="boot_on">1 boot ON</button>
			<button class="btn btn-warning btn-sm" name="action" value="boot_off">0 boot OFF</button>
			<span class="help-block"><?=gettext("Offload at boot: writes hw.xgs_octep.offload in /boot/loader.conf.local; applies from the next boot until changed back.")?> <?=gettext("Now:")?> <?=htmlspecialchars($loader_val)?></span>
		</form>
	</div>
</div>

<?php if (count($facilities)): ?>
<div class="panel panel-default">
	<div class="panel-heading"><h2 class="panel-title"><?=gettext("Facilities (BAR2 offset + size)")?></h2></div>
	<div class="panel-body table-responsive">
		<table class="table table-striped table-hover">
			<tbody>
<?php foreach ($facilities as $name => $where): ?>
				<tr><td><?=htmlspecialchars($name)?></td><td><?=htmlspecialchars($where)?></td></tr>
<?php endforeach; ?>
			</tbody>
		</table>
	</div>
</div>
<?php endif; ?>

<div class="panel panel-default">
	<div class="panel-heading"><h2 class="panel-title"><?=gettext("Diagnostics")?></h2></div>
	<div class="panel-body">
		<pre><?=htmlspecialchars("user=" . xgs_run("/usr/bin/id -un") . "  repo=" . $repo . "  status rc=" . var_export($st_rc, true) . "\n" . ($raw === "" ? "(status empty)" : substr($raw, 0, 300)))?></pre>
	</div>
</div>

<form method="post" action="xgs_octep_status.php">
<div class="panel panel-default">
	<div class="panel-heading"><h2 class="panel-title"><?=gettext("Testing")?></h2></div>
	<div class="panel-body">
		<div class="form-group">
			<label><?=gettext("pcap file name (saved in /home/pcaps/, cut-in only; blank = auto)")?></label>
			<input class="form-control" type="text" id="pcapname" name="pcapname" placeholder="cutin1" maxlength="64" value="<?=htmlspecialchars($_POST["pcapname"] ?? "")?>">
			<label><?=gettext("pcap time limit (seconds, 5 to 600; every capture stops by itself at this limit)")?></label>
			<input class="form-control" type="number" id="pcaplimit" name="pcaplimit" min="5" max="600" step="1" style="max-width:10em" value="<?=xgs_pcap_limit($_POST["pcaplimit"] ?? 120)?>">
			<p>
				<button class="btn btn-primary btn-sm" name="action" value="pcap_start"><?=gettext("Start pcap")?></button>
				<button class="btn btn-danger btn-sm" name="action" value="pcap_stop"><?=gettext("Stop pcap")?></button>
				&nbsp; <?=count(xgs_pcap_pids()) > 0 ? "<span class=\"label label-success\">pcap: RUNNING</span>" : "<span class=\"label label-default\">pcap: stopped</span>"?>
			</p>
			<p class="help-block" id="pcapnote"><?=gettext("The name stays until you change it. Files: /home/pcaps/NAME-octep0.pcap and NAME-octep1.pcap (newest 4 kept).")?></p>
		</div>

		<h4><?=gettext("1. Offload switches")?></h4>
		<table class="table table-condensed">
			<tbody>
				<tr><td><button class="btn btn-success btn-sm" name="action" value="live_on">1 live ON</button></td><td><?=gettext("Gate on now (fw_cfg 0x3). Live ON can run at about 50% rate.")?></td></tr>
				<tr><td><button class="btn btn-warning btn-sm" name="action" value="live_off">0 live OFF</button></td><td><?=gettext("Gate off now (fw_cfg 0x2): host forwards all. Use for baselines.")?></td></tr>
				<tr><td><button class="btn btn-success btn-sm" name="action" value="boot_on">1 boot ON</button></td><td><?=gettext("Writes hw.xgs_octep.offload=1; applies next boot (reference state).")?></td></tr>
				<tr><td><button class="btn btn-warning btn-sm" name="action" value="boot_off">0 boot OFF</button></td><td><?=gettext("Writes hw.xgs_octep.offload=0; applies next boot.")?></td></tr>
			</tbody>
		</table>

		<h4><?=gettext("2. Tests (offload-test.sh)")?></h4>
		<table class="table table-condensed">
			<tbody>
				<tr><td><button class="btn btn-primary btn-sm" name="action" value="boot">boot</button></td><td><?=gettext("Offload on from boot; PASS = WIRE_TO_WIRE and TO_KN_MFLOW_NOT_ACTIVE counters rise.")?></td></tr>
				<tr><td><button class="btn btn-primary btn-sm" name="action" value="sysctl">sysctl</button></td><td><?=gettext("Same proof after a live gate switch.")?></td></tr>
				<tr><td><button class="btn btn-primary btn-sm" name="action" value="cutin">cutin</button></td><td><?=gettext("Programs 1 flow into the NPU 8 s after READY (start iperf then); samples 40 x 0.25 s.")?></td></tr>
				<tr><td><button class="btn btn-default btn-sm" name="action" value="plain">plain</button></td><td><?=gettext("Baseline, no gate check (offload-off comparison).")?></td></tr>
			</tbody>
		</table>

		<h4><?=gettext("3. iperf3")?></h4>
		<div class="row">
			<div class="col-sm-2"><select class="form-control" name="iperf_from"><?php $sf = $_POST["iperf_from"] ?? "octep0"; foreach (array("octep0" => "from octep0 (LAN)", "octep1" => "from octep1 (WAN)", "custom" => "custom IP") as $k => $v) { echo "<option value=\"" . $k . "\"" . ($sf === $k ? " selected" : "") . ">" . $v . "</option>"; } ?></select></div>
			<div class="col-sm-2"><input class="form-control" type="text" name="iperf_bind" value="<?=htmlspecialchars($_POST["iperf_bind"] ?? "")?>" placeholder="custom -B IP"></div>
			<div class="col-sm-2"><input class="form-control" type="text" name="iperf_srv" value="<?=htmlspecialchars($_POST["iperf_srv"] ?? "192.168.1.1")?>" placeholder="server IP"></div>
			<div class="col-sm-2"><input class="form-control" type="number" name="iperf_secs" value="<?=htmlspecialchars($_POST["iperf_secs"] ?? "30")?>" min="1" max="300"></div>
			<div class="col-sm-4">
				<button class="btn btn-primary btn-sm" name="action" value="iperf_client"><?=gettext("Start client")?></button>
				<button class="btn btn-default btn-sm" name="action" value="iperf_server"><?=gettext("Local server")?></button>
				<button class="btn btn-danger btn-sm" name="action" value="iperf_stop"><?=gettext("Stop iperf")?></button>
			</div>
		</div>
		<p class="help-block"><?=gettext("Client: this box (source octep0, octep1 or custom IP) to the server, 1-300 s; box-originated traffic is not forwarded, so it is not offloaded. Local server: accepts 1 test from a LAN host, then exits. The most realistic test is iperf from a LAN client.")?></p>
<?php if ($iperf_rate !== ""): ?>
		<pre><?=htmlspecialchars("last iperf: " . $iperf_rate)?></pre>
<?php endif; ?>
	</div>
</div>
</form>

<div class="panel panel-default">
	<div class="panel-heading"><h2 class="panel-title"><?=gettext("Live packet summary (1 s refresh)")?></h2></div>
	<div class="panel-body table-responsive">
		<button type="button" class="btn btn-default btn-sm" id="xgs-reset"><?=gettext("Reset counts")?></button>
		<p><strong><?=gettext("Flow")?>:</strong> <span id="f-state" class="label label-default">IDLE</span>
			&nbsp; <?=gettext("cut-in seen")?>: <span id="f-cut">-</span>
			&nbsp; <?=gettext("now")?>: <span id="f-now" class="label label-default">-</span>
			&nbsp; <?=gettext("stalls")?>: <span id="f-n">0</span>
			&nbsp; <?=gettext("longest stall (s)")?>: <span id="f-max">0</span>
			&nbsp; <?=gettext("driver stalls / max s")?>: <span id="f-drv">-</span></p>
		<table class="table table-striped table-hover">
			<thead><tr><th></th><th><?=gettext("pkts since reset")?></th><th><?=gettext("pkts/s")?></th><th><?=gettext("max pkts/s")?></th><th><?=gettext("min pkts/s")?></th><th><?=gettext("est. Mbit/s")?></th></tr></thead>
			<tbody>
				<tr><td><?=gettext("Offloaded (NPU wire-to-wire)")?></td><td id="c-off">-</td><td id="r-off">-</td><td id="x-off">-</td><td id="n-off">-</td><td id="m-off">-</td></tr>
				<tr><td><?=gettext("Not offloaded (host path)")?></td><td id="c-no">-</td><td id="r-no">-</td><td id="x-no">-</td><td id="n-no">-</td><td id="m-no">-</td></tr>
				<tr><td>octep0 LAN (host in+out)</td><td id="c-o0">-</td><td id="r-o0">-</td><td id="x-o0">-</td><td id="n-o0">-</td><td>-</td></tr>
				<tr><td>octep1 WAN (host in+out)</td><td id="c-o1">-</td><td id="r-o1">-</td><td id="x-o1">-</td><td id="n-o1">-</td><td>-</td></tr>
			</tbody>
		</table>
		<p class="help-block"><?=sprintf(gettext("Mbit/s is estimated at %d bytes per frame (the driver keeps no byte counters). The measured figure is the iperf result."), XGS_FRAME_BYTES)?></p>
	</div>
</div>

<div class="panel panel-default">
	<div class="panel-heading"><h2 class="panel-title"><?=gettext("Recent logs")?></h2></div>
	<div class="panel-body">
<?php foreach ($logs as $l): ?>
		<a href="?log=<?=urlencode(basename($l))?>"><?=htmlspecialchars(basename($l))?></a><br>
<?php endforeach; ?>
<?php if ($viewtext !== ""): ?>
		<pre style="max-height:400px;overflow:auto"><?=htmlspecialchars($viewtext)?></pre>
<?php endif; ?>
	</div>
</div>

<script type="text/javascript">
//<![CDATA[
(function () {
	var pn = document.getElementById("pcapname"), note = document.getElementById("pcapnote");
	function pcapNote() {
		note.textContent = pn.value === "" ? "Blank = auto name (date-time). Files go to /home/pcaps/ (newest 4 kept)." : "Files: /home/pcaps/" + pn.value + "-octep0.pcap and /home/pcaps/" + pn.value + "-octep1.pcap (newest 4 kept). The name stays until you change it.";
	}
	try {
		if (pn.value === "") { pn.value = localStorage.getItem("xgsPcapName") || ""; }
		pn.addEventListener("keydown", function (e) { if (e.key === "Enter") { e.preventDefault(); } });
		pn.addEventListener("input", function () { localStorage.setItem("xgsPcapName", pn.value); pcapNote(); });
	} catch (e) {}
	pcapNote();
	try {
		["iperf_from", "iperf_bind", "iperf_srv", "iperf_secs", "pcaplimit"].forEach(function (n) {
			var e = document.getElementsByName(n)[0], k = "xgs_" + n;
			if (!e) { return; }
			var v = localStorage.getItem(k);
			if (v !== null && <?=isset($_POST["iperf_from"]) ? "false" : "true"?>) { e.value = v; }
			e.addEventListener("change", function () { localStorage.setItem(k, e.value); });
			e.addEventListener("input", function () { localStorage.setItem(k, e.value); });
		});
	} catch (e) {}
})();
(function () {
	var FRAME = <?=XGS_FRAME_BYTES?>, KEYS = ["off", "no", "o0", "o1"], LOW = 10, BUSY = 100;
	var base, prev, mx, mn, st, KEY = "xgsFlow";
	function fresh() {
		base = null; prev = null; mx = {}; mn = {};
		st = {seen: false, t0: null, cut: null, low: 0, n: 0, max: 0};
		KEYS.forEach(function (k) { mx[k] = 0; mn[k] = null; });
	}
	function save() {
		try { sessionStorage.setItem(KEY, JSON.stringify({base: base, mx: mx, mn: mn, st: st})); } catch (e) {}
	}
	function load() {
		fresh();
		try {
			var d = JSON.parse(sessionStorage.getItem(KEY));
			if (d && d.st && d.mx) { base = d.base; mx = d.mx; mn = d.mn; st = d.st; }
		} catch (e) {}
	}
	function reset() { fresh(); save(); }
	function vals(s) {
		return {
			t: s.t,
			off: s.offloaded,
			no: s.to_host + s.not_active + s.non_accel,
			o0: s.octep0["in"] + s.octep0.out,
			o1: s.octep1["in"] + s.octep1.out
		};
	}
	function set(id, v) { document.getElementById(id).textContent = v; }
	function badge(id, text, cls) {
		var e = document.getElementById(id);
		e.textContent = text;
		e.className = "label label-" + cls;
	}
	function flow(r, now) {
		var total = r.off + r.no + r.o0 + r.o1, label, cls;
		if (total >= BUSY) { st.seen = true; if (st.t0 === null) { st.t0 = now; } }
		if (r.off >= BUSY && st.cut === null) { st.cut = ((now - st.t0) || 0).toFixed(0) + " s after first traffic"; }
		if (st.seen && total < LOW) {
			st.low++;
		} else if (total >= LOW) {
			if (st.low >= 2 && st.low <= 10) {
				st.n++;
				if (st.low > st.max) { st.max = st.low; }
			}
			st.low = 0;
		}
		if (st.seen && st.low >= 2 && st.low <= 10) { label = "STALLED"; cls = "danger"; }
		else if (r.off >= BUSY) { label = "OFFLOADED"; cls = "success"; }
		else if (r.no >= BUSY || r.o0 + r.o1 >= BUSY) { label = "HOST"; cls = "warning"; }
		else { label = "IDLE"; cls = "default"; }
		badge("f-state", label, cls);
		badge("f-now", Math.round(total) + " pkts/s", cls);
		set("f-cut", st.cut === null ? "-" : st.cut);
		set("f-n", st.n);
		set("f-max", st.max);
	}
	function tick() {
		fetch("xgs_octep_status.php?ajax=1", {credentials: "same-origin"}).then(function (r) { return r.json(); }).then(function (s) {
			if (s.error) { return; }
			var v = vals(s), r = {};
			set("f-drv", s.drv_stalls + " / " + s.drv_stall_max);
			if (!base || v.off < base.off || v.no < base.no || v.o0 < base.o0 || v.o1 < base.o1) { base = v; }
			KEYS.forEach(function (k) {
				set("c-" + k, v[k] - base[k]);
				if (prev) {
					var pps = Math.round((v[k] - prev[k]) / (v.t - prev.t));
					r[k] = pps;
					set("r-" + k, pps);
					if (pps > mx[k]) { mx[k] = pps; }
					if (pps > 0 && (mn[k] === null || pps < mn[k])) { mn[k] = pps; }
					set("x-" + k, mx[k]);
					set("n-" + k, mn[k] === null ? "-" : mn[k]);
					if (k === "off" || k === "no") { set("m-" + k, (pps * FRAME * 8 / 1e6).toFixed(0)); }
				}
			});
			if (prev) { flow(r, v.t); }
			prev = v;
			save();
		}).catch(function () {});
	}
	load();
	document.getElementById("xgs-reset").addEventListener("click", reset);
	tick();
	setInterval(tick, 1000);
})();
//]]>
</script>

<?php include("foot.inc"); ?>
