<?php
// SPDX-License-Identifier: GPL-2.0-or-later

##|+PRIV
##|*IDENT=page-status-xgs-npu
##|*NAME=Status: XGS NPU
##|*DESCR=Allow access to the XGS NPU status page.
##|*MATCH=xgs_octep_status.php*
##|-PRIV

require_once("guiconfig.inc");

function xgs_run($cmd) {
	return trim((string)shell_exec($cmd . " 2>/dev/null"));
}

$loaded = xgs_run("/sbin/kldstat -q -n xgs_octep && echo yes") === "yes";
$present = xgs_run("/usr/sbin/pciconf -l | /usr/bin/grep -c -E 'chip=0xa300177d|vendor=0x177d[[:space:]]+device=0xa300'") !== "0";
$raw = $loaded ? xgs_run("/sbin/sysctl -n dev.xgs_octep.0.status") : "";

$kv = array();
$facilities = array();
foreach (preg_split('/\s+/', $raw, -1, PREG_SPLIT_NO_EMPTY) as $word) {
	if (strpos($word, "=") === false) {
		continue;
	}
	list($k, $v) = explode("=", $word, 2);
	if (in_array($k, array("control", "mgmt_netdev", "nw_agent", "rpc", "giu"))) {
		$facilities[$k] = $v;
	} else {
		$kv[$k] = $v;
	}
}

$ready = isset($kv["ready"]) && $kv["ready"] === "1";
$nwa = isset($kv["nwa"]) && $kv["nwa"] === "1";

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

$pgtitle = array(gettext("Status"), gettext("XGS NPU"));
include("head.inc");
?>

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
				<tr><td><?=gettext("Ports reported")?></td><td><?=htmlspecialchars($kv["ports"] ?? "-")?></td></tr>
				<tr><td><?=gettext("Last error")?></td><td><?=htmlspecialchars($kv["last_err"] ?? "-")?></td></tr>
			</tbody>
		</table>
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

<?php include("foot.inc"); ?>
