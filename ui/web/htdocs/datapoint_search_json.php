<?php

require_once('Reconnoiter_DB.php');

$db = Reconnoiter_DB::GetDB();

header('Content-Type: application/json; charset=utf-8');

try {
  print json_encode($db->get_datapoints($_GET['q'], $_GET['o'], $_GET['l']));
}
catch(Exception $e) {
  print json_encode(array(
    'count' => 0,
    'query' => $_GET['q'],
    'offset' => $_GET['o'],
    'limit' => $_GET['l'],
    'error' => $e->getMessage()
  ));
}
