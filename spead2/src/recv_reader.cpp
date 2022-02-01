/* Copyright 2015, 2019 National Research Foundation (SARAO)
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 */

#include <spead2/recv_reader.h>
#include <spead2/recv_stream.h>

namespace spead2
{
namespace recv
{

void reader::stopped()
{
    // Schedule it to run later so that at the time it occurs there are no
    // further references to *this.
    stream *owner_ptr = &owner;
    get_io_service().post([owner_ptr] { owner_ptr->readers_stopped.put(); });
}

bool reader::lossy() const
{
    return true;
}

boost::asio::io_service &reader::get_io_service()
{
    return owner.get_io_service();
}

stream_base &reader::get_stream_base() const
{
    return owner;
}

} // namespace recv
} // namespace spead2
