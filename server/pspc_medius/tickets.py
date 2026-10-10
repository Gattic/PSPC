# SPDX-License-Identifier: GPL-2.0-or-later
"""Who is logging in: the NP ticket a game sends with MediusTicketLogin, checked with the PSPC
Network server that issued it (POST /v1/tickets/verify). Without a PSPC Network server, or for
players who sign in locally (no server in their pspc.ini), local tickets can be let in by name."""

import asyncio
import base64
import json
import logging
import os
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from pspc_network import ticket as np_ticket  # noqa: E402

log = logging.getLogger('medius.tickets')


def find_ticket(data):
    """The NP ticket inside a message: a version 2.x/3.x/4.x header followed by its size."""
    for start in range(0, max(0, len(data) - 12)):
        if data[start] in (0x21, 0x31, 0x41) and data[start + 1] in (0, 1) and data[start + 2] == 0 and data[start + 3] == 0:
            size = int.from_bytes(data[start + 4:start + 8], 'big')
            if 16 <= size <= len(data) - start - 8:
                blob = bytes(data[start:start + 8 + size])
                try:
                    np_ticket.parse(blob)
                    return start, blob
                except np_ticket.TicketError:
                    continue
    return -1, None


class TicketVerifier:
    def __init__(self, network_url=None, allow_local=False, service_ids=()):
        self.network_url = (network_url or '').rstrip('/')
        self.allow_local = allow_local
        self.service_ids = set(service_ids)

    async def verify(self, blob):
        """(online ID, None) or (None, reason)."""
        try:
            fields = np_ticket.parse(blob)
        except np_ticket.TicketError as e:
            return None, 'malformed ticket: %s' % e
        if self.service_ids and fields['service_id'] not in self.service_ids:
            return None, 'ticket for another game (%s)' % fields['service_id']
        if fields['cipher'] == np_ticket.CIPHER_LOCAL:
            if self.allow_local:
                return fields['online_id'], None
            return None, 'local sign-in not allowed on this server (sign in to its PSPC Network server)'
        if not self.network_url:
            if self.allow_local:
                return fields['online_id'], None
            return None, 'this server has no PSPC Network server to check tickets with'
        try:
            result = await asyncio.to_thread(self._post, blob)
        except (OSError, ValueError) as e:
            log.warning('PSPC Network server %s: %s', self.network_url, e)
            return None, "can't reach the PSPC Network server"
        if not result.get('valid'):
            return None, result.get('reason') or 'invalid ticket'
        return result['online_id'], None

    def _post(self, blob):
        body = json.dumps({'ticket': base64.b64encode(blob).decode('ascii')}).encode()
        req = urllib.request.Request(self.network_url + '/v1/tickets/verify', data=body, method='POST',
                                     headers={'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return json.loads(r.read() or b'{}')
        except urllib.error.HTTPError as e:
            return json.loads(e.read() or b'{}')
