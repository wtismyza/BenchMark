# Copyright 2015, 2019-2020 National Research Foundation (SARAO)
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option) any
# later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
# details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

"""
Integration between spead2.send and asyncio
"""

import asyncio


from spead2._spead2.send import UdpStreamAsyncio as _UdpStreamAsyncio
from spead2._spead2.send import TcpStreamAsyncio as _TcpStreamAsyncio
from spead2._spead2.send import InprocStreamAsyncio as _InprocStreamAsyncio


def _wrap_class(name, base_class):
    class Wrapped(base_class):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            self._active = 0
            self._last_queued_future = None

        def _async_send(self, call):
            future = asyncio.Future()
            loop = asyncio.get_event_loop()

            def callback(exc, bytes_transferred):
                if exc is not None:
                    future.set_exception(exc)
                else:
                    future.set_result(bytes_transferred)
                self._active -= 1
                if self._active == 0:
                    loop.remove_reader(self.fd)
                    self._last_queued_future = None  # Purely to free the memory
            queued = call(callback)
            if self._active == 0:
                loop.add_reader(self.fd, self.process_callbacks)
            self._active += 1
            if queued:
                self._last_queued_future = future
            return future

        def async_send_heap(self, heap, cnt=-1, substream_index=0):
            """Send a heap asynchronously. Note that this is *not* a coroutine:
            it returns a future. Adding the heap to the queue is done
            synchronously, to ensure proper ordering.

            Parameters
            ----------
            heap : :py:class:`spead2.send.Heap`
                Heap to send
            cnt : int, optional
                Heap cnt to send (defaults to auto-incrementing)
            substream_index : int, optional
                Substream on which to send the heap
            """
            meth = super().async_send_heap
            return self._async_send(
                lambda callback: meth(heap, callback, cnt, substream_index))

        def async_send_heaps(self, heaps, mode):
            meth = super().async_send_heaps
            return self._async_send(
                lambda callback: meth(heaps, callback, mode))

        async def async_flush(self):
            """Asynchronously wait for all enqueued heaps to be sent. Note that
            this only waits for heaps passed to :meth:`async_send_heap` prior to
            this call, not ones added while waiting."""
            future = self._last_queued_future
            if future is not None:
                await asyncio.wait([future])

    Wrapped.__name__ = name
    return Wrapped


UdpStream = _wrap_class('UdpStream', _UdpStreamAsyncio)
UdpStream.__doc__ = \
    """SPEAD over UDP with asynchronous sends. The other constructors
    defined for :py:class:`spead2.send.UdpStream` are also applicable here.

    Parameters
    ----------
    thread_pool : :py:class:`spead2.ThreadPool`
        Thread pool handling the I/O
    endpoints : List[Tuple[str, int]]
        Peer endpoints (one per substreams).
    config : :py:class:`spead2.send.StreamConfig`
        Stream configuration
    buffer_size : int
        Socket buffer size. A warning is logged if this size cannot be set due
        to OS limits.
    """

_TcpStreamBase = _wrap_class('TcpStream', _TcpStreamAsyncio)


class TcpStream(_TcpStreamBase):
    """SPEAD over TCP with asynchronous connect and sends.

    Most users will use :py:meth:`connect` to asynchronously create a stream.
    The constructor should only be used if you wish to provide your own socket
    and take care of connecting yourself.

    Parameters
    ----------
    thread_pool : :py:class:`spead2.ThreadPool`
        Thread pool handling the I/O
    socket : :py:class:`socket.socket`
        TCP/IP Socket that is already connected to the remote end
    config : :py:class:`spead2.send.StreamConfig`
        Stream configuration
    """

    @classmethod
    async def connect(cls, *args, **kwargs):
        """Open a connection.

        The arguments are the same as for the constructor of
        :py:class:`spead2.send.TcpStream`.
        """
        future = asyncio.Future()
        loop = asyncio.get_event_loop()

        def callback(arg):
            if not future.done():
                if isinstance(arg, Exception):
                    loop.call_soon_threadsafe(future.set_exception, arg)
                else:
                    loop.call_soon_threadsafe(future.set_result, arg)

        stream = cls(callback, *args, **kwargs)
        await future
        return stream


InprocStream = _wrap_class('InprocStream', _InprocStreamAsyncio)
InprocStream.__doc__ = \
    """SPEAD over reliable in-process transport.

    .. note::

        Data may still be lost if the maximum number of in-flight heaps (set
        in the stream config) is exceeded. Either set this value to more
        heaps than will ever be sent (which will use unbounded memory) or be
        sure to block on the futures returned before exceeding the capacity.

    Parameters
    ----------
    thread_pool : :py:class:`spead2.ThreadPool`
        Thread pool handling the I/O
    queues : List[:py:class:`spead2.InprocQueue`]
        Queue holding the data in flight
    config : :py:class:`spead2.send.StreamConfig`
        Stream configuration
    """

try:
    from spead2._spead2.send import UdpIbvStreamAsyncio as _UdpIbvStreamAsyncio

    UdpIbvStream = _wrap_class('UdpIbvStream', _UdpIbvStreamAsyncio)
    UdpIbvStream.__doc__ = \
        """Like :class:`UdpStream`, but using the Infiniband Verbs API.

        Parameters
        ----------
        thread_pool : :py:class:`spead2.ThreadPool`
            Thread pool handling the I/O
        endpoints : List[Tuple[str, int]]
            Destinations to transmit to. For backwards compatibility, one can
            also provide a single address and port as two separate
            parameters.
        config : :py:class:`spead2.send.StreamConfig`
            Stream configuration
        interface_address : str
            IP address of network interface from which to send
        buffer_size : int, optional
            Buffer size
        ttl : int, optional
            Time-To-Live of packets
        comp_vector : int, optional
            Completion channel vector (interrupt)
            for asynchronous operation, or
            a negative value to poll continuously. Polling
            should not be used if there are other users of the
            thread pool. If a non-negative value is provided, it
            is taken modulo the number of available completion
            vectors. This allows a number of readers to be
            assigned sequential completion vectors and have them
            load-balanced, without concern for the number
            available.
        max_poll : int
            Maximum number of times to poll in a row, without
            waiting for an interrupt (if `comp_vector` is
            non-negative) or letting other code run on the
            thread (if `comp_vector` is negative).
        """

except ImportError:
    pass
