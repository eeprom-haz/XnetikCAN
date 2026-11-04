#pragma once
#include <QCanBusFrame>
#include <QMutex>
#include <QMutexLocker>
#include <QVector>

class RingBuffer {
public:
    explicit RingBuffer(int capacity = 100000) : m_capacity(capacity) {
        m_buf.resize(capacity);
    }

    void push(const QCanBusFrame &frame) {
        QMutexLocker lk(&m_mutex);
        m_buf[m_head] = frame;
        m_head = (m_head + 1) % m_capacity;
        if (m_count < m_capacity) ++m_count;
    }

    // Returns up to maxCount frames, newest first.
    QVector<QCanBusFrame> snapshot(int maxCount = -1) const {
        QMutexLocker lk(&m_mutex);
        int n = (maxCount < 0 || maxCount > m_count) ? m_count : maxCount;
        QVector<QCanBusFrame> out(n);
        int idx = (m_head - 1 + m_capacity) % m_capacity;
        for (int i = 0; i < n; ++i) {
            out[i] = m_buf[idx];
            idx = (idx - 1 + m_capacity) % m_capacity;
        }
        return out;
    }

    // Returns all frames in chronological order.
    QVector<QCanBusFrame> all() const {
        QMutexLocker lk(&m_mutex);
        QVector<QCanBusFrame> out(m_count);
        int start = (m_count < m_capacity) ? 0 : m_head;
        for (int i = 0; i < m_count; ++i)
            out[i] = m_buf[(start + i) % m_capacity];
        return out;
    }

    void clear() {
        QMutexLocker lk(&m_mutex);
        m_count = 0;
        m_head = 0;
    }

    int size() const {
        QMutexLocker lk(&m_mutex);
        return m_count;
    }

private:
    mutable QMutex m_mutex;
    QVector<QCanBusFrame> m_buf;
    int m_capacity;
    int m_head = 0;
    int m_count = 0;
};
