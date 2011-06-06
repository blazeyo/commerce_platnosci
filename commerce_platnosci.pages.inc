<?php

/**
 * Menu callback.
 * It gets called by platnosci.pl to let us know that some ordder's payment status was updated.
 */
function commerce_platnosci_payment_update() {
  // Load payment rule settings.
  $methods->payment_methods = array();
  rules_invoke_all('commerce_payment_methods', $methods);
  $settings = $methods->payment_methods['commerce_platnosci|commerce_payment_commerce_platnosci']['settings'];

  if (!empty($settings)) {
    // Validate packet signature.
    $sig_check = md5($settings['pos_id'] . $_POST['session_id'] . $_POST['ts'] . $settings['key2']);
    if ($sig_check == $_POST['sig']) {
      // Fetch transaction status and update our local copy.
      $result = commerce_platnosci_fetch_status($_POST['session_id'], $settings);
      commerce_platnosci_transaction_update($result, $settings);
    }
  }

  // Tell gateway that we received the message.
  print 'OK';
  exit();
}

/**
 * This function sends an update request to platnosci.pl after we have been
 * notified that something has changed. It resurns parsed response from paygw.
 *
 * @param int $order_id
 * @param array $settings
 * @return array
 */
function commerce_platnosci_fetch_status($session_id, $settings) {
  $url = 'https://www.platnosci.pl/paygw/UTF/Payment/get/txt';
  $ts = time();

  // Calculate request signature
  $sig = md5($settings['pos_id'] . $session_id . $ts . $settings['key1']);

  // Translate post variables into urlencoded query string. I don't know why but
  // passing an array does not work in my environment.
  $postfields = array(
    'pos_id' => $settings['pos_id'],
    'session_id' => $session_id,
    'ts' => $ts,
    'sig' => $sig,
  );
  $output = '';
  foreach ($postfields as $key => $value) {
    $output .= urlencode($key) . '=' . urlencode($value) . '&';
  }
  rtrim($output, '&');

  // Set up curl options and send request.
  $ch = curl_init();
  curl_setopt($ch, CURLOPT_URL, $url);
  curl_setopt($ch, CURLOPT_POST, true);
  curl_setopt($ch, CURLOPT_POSTFIELDS, $output);
  curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
  curl_setopt($ch, CURLOPT_HEADER, true);
  $result = curl_exec($ch);
  curl_close($ch);

  // Return gateway response parsed to array
  return commerce_platnosci_parse_response($result);
}

/**
 * We translate geteway response from string to structured array.
 *
 * @param string $response
 * @return array
 */
function commerce_platnosci_parse_response($response) {
  $response_data = array();
  foreach (explode("\n", trim($response)) as $line) {
    $row = explode(":", $line);
    if (count($row) > 1) {
      $response_data[trim($row[0])] = trim($row[1]);
    }
  }
  return $response_data;
}

/**
 * Creates or updates our transaction based on platnosci.pl response.
 *
 * @param array $response
 * @param stdClass $order
 * @param array $settings
 */
function commerce_platnosci_transaction_update($response, $settings) {
  // Verify packet signature
  $sig =  md5(
          $response['trans_pos_id'] . $response['trans_session_id'] .
          $response['trans_order_id'] . $response['trans_status'] .
          $response['trans_amount'] . $response['trans_desc'] .
          $response['trans_ts'] . $settings['key2']);

  if ($sig == $response['trans_sig']) {
    $order = commerce_order_load($response['trans_order_id']);
    $transaction = commerce_payment_transaction_load((int) $response['trans_desc2']);

    $trans_status = (int) $response['trans_status'];
    switch ($trans_status) {
      case 99: // return 'płatność odbrana - zakończona';
        $status = COMMERCE_PAYMENT_STATUS_SUCCESS;
        $message = 'Complete';
        break;

      case 1: // return 'Nowa';
      case 4: // return 'Rozpoczęta';
      case 5: // return 'Oczekuje na odbiór';
        $status = COMMERCE_PAYMENT_STATUS_PENDING;
        $message = 'Processing';
        break;

      case 2: // return 'Anulowana';
      case 3: // return 'Odrzucona';
      case 7: // return 'płatność odrzucona, otrzymano środki od klienta po wcześniejszym anulowaniu transakcji, lub nie było możliwości zwrotu środków w sposób automatyczny';
      default: // return 'błędny status - prosimy o kontakt z platnosci.pl';
        $status = COMMERCE_PAYMENT_STATUS_FAILURE;
        $message = 'Failed';
        break;
    }

    $transaction->status = $status;
    $transaction->message = $message;
    $transaction->amount = $response['trans_amount'];
    commerce_payment_transaction_save($transaction);
    commerce_order_save($order);
  }
}
