#include "exchange/multicast_publisher.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#ifdef _WIN32
#error "Windows support not implemented yet."
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace exchange {

struct MulticastPublisher::Destination {
    sockaddr_in address {};
};

MulticastPublisher::MulticastPublisher(
    MulticastConfig config
)
    : config_(std::move(config)) {}

MulticastPublisher::~MulticastPublisher() {
    stop();
}

bool MulticastPublisher::start() {
    if (running_.load()) {
        return true;
    }

    socket_ = ::socket(
        AF_INET,
        SOCK_DGRAM,
        0
    );

    if (socket_ < 0) {
        return false;
    }

    const unsigned char ttl =
        static_cast<unsigned char>(
            config_.ttl
        );

    if (
        ::setsockopt(
            socket_,
            IPPROTO_IP,
            IP_MULTICAST_TTL,
            &ttl,
            sizeof(ttl)
        ) != 0
    ) {
        ::close(socket_);
        socket_ = -1;
        return false;
    }

    const unsigned char loopback = 1;

    if (
        ::setsockopt(
            socket_,
            IPPROTO_IP,
            IP_MULTICAST_LOOP,
            &loopback,
            sizeof(loopback)
        ) != 0
    ) {
        ::close(socket_);
        socket_ = -1;
        return false;
    }

    auto destination =
        std::make_unique<Destination>();

    destination->address.sin_family = AF_INET;
    destination->address.sin_port =
        htons(config_.port);

    if (
        ::inet_pton(
            AF_INET,
            config_.group.c_str(),
            &destination->address.sin_addr
        ) != 1
    ) {
        ::close(socket_);
        socket_ = -1;
        return false;
    }

    destination_ = destination.release();

    {
        std::lock_guard<std::mutex> lock(
            queue_mutex_
        );

        read_index_ = 0;
        write_index_ = 0;
        queued_count_ = 0;
    }

    enqueued_.store(0);
    sent_.store(0);
    dropped_.store(0);
    send_errors_.store(0);

    running_.store(true);

    try {
        publisher_thread_ = std::thread(
            &MulticastPublisher::publisher_loop,
            this
        );
    } catch (...) {
        running_.store(false);

        delete destination_;
        destination_ = nullptr;

        ::close(socket_);
        socket_ = -1;

        return false;
    }

    return true;
}

void MulticastPublisher::stop() noexcept {
    const bool was_running =
        running_.exchange(false);

    if (was_running) {
        queue_cv_.notify_all();
    }

    if (publisher_thread_.joinable()) {
        publisher_thread_.join();
    }

    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }

    delete destination_;
    destination_ = nullptr;

    {
        std::lock_guard<std::mutex> lock(
            queue_mutex_
        );

        read_index_ = 0;
        write_index_ = 0;
        queued_count_ = 0;
    }
}

bool MulticastPublisher::send(
    std::span<const std::byte> datagram
) noexcept {
    if (
        !running_.load() ||
        datagram.empty() ||
        datagram.size() >
            kMaximumDatagramSize
    ) {
        dropped_.fetch_add(
            1,
            std::memory_order_relaxed
        );

        return false;
    }

    {
        std::lock_guard<std::mutex> lock(
            queue_mutex_
        );

        if (
            !running_.load() ||
            queued_count_ == kQueueCapacity
        ) {
            dropped_.fetch_add(
                1,
                std::memory_order_relaxed
            );

            return false;
        }

        Slot& slot = queue_[write_index_];

        std::memcpy(
            slot.bytes.data(),
            datagram.data(),
            datagram.size()
        );

        slot.size = datagram.size();

        write_index_ =
            (write_index_ + 1U) %
            kQueueCapacity;

        ++queued_count_;
    }

    enqueued_.fetch_add(
        1,
        std::memory_order_relaxed
    );

    queue_cv_.notify_one();

    return true;
}

bool MulticastPublisher::is_open() const noexcept {
    return running_.load() &&
        socket_ >= 0 &&
        destination_ != nullptr;
}

const MulticastConfig&
MulticastPublisher::config() const noexcept {
    return config_;
}

MulticastPublisherStats
MulticastPublisher::stats() const noexcept {
    return {
        .enqueued = enqueued_.load(
            std::memory_order_relaxed
        ),
        .sent = sent_.load(
            std::memory_order_relaxed
        ),
        .dropped = dropped_.load(
            std::memory_order_relaxed
        ),
        .send_errors = send_errors_.load(
            std::memory_order_relaxed
        )
    };
}

void MulticastPublisher::publisher_loop() noexcept {
    while (true) {
        Slot local_slot;

        {
            std::unique_lock<std::mutex> lock(
                queue_mutex_
            );

            queue_cv_.wait(
                lock,
                [this]() {
                    return
                        queued_count_ > 0 ||
                        !running_.load();
                }
            );

            if (
                queued_count_ == 0 &&
                !running_.load()
            ) {
                break;
            }

            Slot& queued_slot =
                queue_[read_index_];

            local_slot.size =
                queued_slot.size;

            std::copy_n(
                queued_slot.bytes.begin(),
                local_slot.size,
                local_slot.bytes.begin()
            );

            queued_slot.size = 0;

            read_index_ =
                (read_index_ + 1U) %
                kQueueCapacity;

            --queued_count_;
        }

        const std::span<const std::byte>
            datagram {
                local_slot.bytes.data(),
                local_slot.size
            };

        if (send_datagram(datagram)) {
            sent_.fetch_add(
                1,
                std::memory_order_relaxed
            );
        } else {
            send_errors_.fetch_add(
                1,
                std::memory_order_relaxed
            );
        }
    }
}

bool MulticastPublisher::send_datagram(
    std::span<const std::byte> datagram
) noexcept {
    if (
        socket_ < 0 ||
        destination_ == nullptr ||
        datagram.empty()
    ) {
        return false;
    }

    const auto sent = ::sendto(
        socket_,
        datagram.data(),
        datagram.size(),
        0,
        reinterpret_cast<const sockaddr*>(
            &destination_->address
        ),
        sizeof(destination_->address)
    );

    return sent ==
        static_cast<ssize_t>(
            datagram.size()
        );
}

}  // namespace exchange