#!/usr/bin/env python3
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from nexus_notifications import transitions


class NotificationTests(unittest.TestCase):
    def test_initial_counter_does_not_replay_old_targets(self):
        state = {}
        self.assertEqual(transitions({'event': 'NEXUS_TELEMETRY', 'counters': {'targets_satisfied': 9}}, state), [])
        self.assertEqual(transitions({'event': 'NEXUS_TELEMETRY', 'counters': {'targets_satisfied': 10}}, state), [])

    def test_counter_reset_is_not_a_victory(self):
        self.assertEqual(transitions({'event': 'NEXUS_TELEMETRY', 'counters': {'targets_satisfied': 0}}, {'targets': 9}), [])

    def test_ack_dag_and_selected_are_distinct_and_deduplicated(self):
        state = {}; block = 'a' * 64
        for event, value, selected, expected in (
            ('LIVE_SUBMIT_RESULT', 'RPC_ACKNOWLEDGED', None, 'SUBMIT_ACK'),
            ('LIVE_CHAIN_STATUS', 'CHAIN_CONFIRMED', 'false', None),
            ('LIVE_CHAIN_STATUS', 'CHAIN_CONFIRMED', 'true', None)):
            row = {'event': event, 'state': value, 'hash': block, 'selected_chain': selected}
            self.assertEqual(transitions(row, state), ([(expected, block)] if expected else []))
            self.assertEqual(transitions(row, state), [])

    def test_pool_receipts_use_job_nonce_instead_of_missing_block_hash(self):
        state = {}
        row = {'event': 'LIVE_SUBMIT_RESULT', 'mode': 'POOL', 'state': 'RPC_ACKNOWLEDGED',
               'hash': '0' * 64, 'job_id': 'synthetic', 'nonce': '1'}
        first = transitions(row, state)
        self.assertEqual(first[0][0], 'POOL_SHARE_ACK')
        self.assertEqual(transitions(row, state), [])
        row['nonce'] = '2'
        second = transitions(row, state)
        self.assertEqual(second[0][0], 'POOL_SHARE_ACK')
        self.assertNotEqual(first[0][1], second[0][1])
        row['job_id'] = ''
        self.assertEqual(transitions(row, state), [])

    def test_h15_solo_ack_is_supported(self):
        self.assertEqual(transitions({'event': 'LIVE_H15_SUBMIT_RESULT',
                                     'state': 'RPC_ACKNOWLEDGED', 'hash': 'b' * 64}, {}),
                         [('SUBMIT_ACK', 'b' * 64)])

    def test_invalid_hash_never_notifies(self):
        self.assertEqual(transitions({'event': 'LIVE_SUBMIT_RESULT', 'state': 'RPC_ACKNOWLEDGED', 'hash': 'bad'}, {}), [])

    def test_submit_sent_is_not_ack(self):
        state = {}; row = {'event': 'NEXUS_TELEMETRY', 'state': {'submission': 'SUBMIT_SENT'}}
        self.assertEqual(transitions(row, state), [])
        self.assertEqual(transitions(row, state), [])


if __name__ == '__main__':
    unittest.main()
