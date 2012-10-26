#!@@PERL@@
# 
# Kill all slon instances for the current cluster
# Author: Christopher Browne
# Copyright 2004-2009 Afilias Canada

use Getopt::Long;

# Defaults
$CONFIG_FILE = '@@SYSCONFDIR@@/slon_tools.conf';
$SHOW_USAGE  = 0;
$WATCHDOG_ONLY = 0;
$ONLY_NODE = 0;

# Read command-line options
GetOptions("config=s"   => \$CONFIG_FILE,
	   "help"       => \$SHOW_USAGE,
	   "w|watchdog" => \$WATCHDOG_ONLY,
	   "only-node=i" => \$ONLY_NODE);

my $USAGE =
"Usage: slon_kill [--config file] [-w|--watchdog] 

    --config file  Location of the slon_tools.conf file

    -w
    --watchdog     Only kill the watchdog process(es)

    Kills all running slon and slon_watchdog on this machine for every
    node in the cluster.

";

if ($SHOW_USAGE) {
  print $USAGE;
  exit 0;
}

require '@@PERLSHAREDIR@@/slon-tools.pm';
require $CONFIG_FILE;

print "slon_kill.pl...   Killing all slon and slon_watchdog instances for the cluster $CLUSTER_NAME\n";
print "1.  Kill slon watchdogs\n";

$found="n";

# kill the watchdogs
if($ONLY_NODE) {
  kill_watchdog($ONLY_NODE);
} else {
  for my $nodenum (@NODES) {
    kill_watchdog($nodenum);
  }
}
if ($found eq 'n') {
    print "No watchdogs found\n";
}

unless ($WATCHDOG_ONLY) {
    print "\n2. Kill slon processes\n";

    # kill the slon daemons
    $found="n";

    if($ONLY_NODE) {
      kill_slon_node( $ONLY_NODE );
    } else {
      for my $nodenum (@NODES) {
        kill_slon_node( $nodenum );
      }
    }

    if ($found eq 'n') {
      print "No slon processes found\n";
    }
}

sub shut_off_processes($$) {
    my ( $watchdog_suffix , $nodenum ) = @_;

    while ($pid = <PSOUT>) {
	chomp $pid;
	if (!($pid)) {
	    print "No slon$watchdog_suffix is running for the cluster $CLUSTER_NAME, node $nodenum!\n";
	} else {
	    $found="y";
	    kill 9, $pid;
	    print "slon$watchdog_suffix for cluster $CLUSTER_NAME node $nodenum killed - PID [$pid]\n";
	}
    }
}

sub kill_watchdog($) {
  my ($nodenum) = @_;

  my $config_regexp = quotemeta( $CONFIG_FILE );

  my $command =  ps_args() . "| egrep \"[s]lon_watchdog[2]? .*=$config_regexp node$nodenum \" | awk '{print \$2}' | sort -n";

  #print "Command:\n$command\n";
  open(PSOUT, "$command|");
  shut_off_processes('_watchdog',$nodenum);
  close(PSOUT);
}

sub kill_slon_node($) {
  my ($nodenum) = @_;

  my $command;
  my ($dsn, $config) = ($DSN[$nodenum], $CONFIG[$nodenum]);
  if ($config) {
    my $config_regexp = quotemeta( $config );
    $command =  ps_args() . "| egrep \"[s]lon -f $config_regexp\" | awk '{print \$2}' | sort -n";
  } else {
    $dsn = quotemeta($dsn);
    $command =  ps_args() . "| egrep \"[s]lon .* $CLUSTER_NAME \" | egrep \"$dsn\" | awk '{print \$2}' | sort -n";
  }
  #print "Command:\n$command\n";
  open(PSOUT, "$command|");
  shut_off_processes("",$nodenum);
  close(PSOUT);
}
