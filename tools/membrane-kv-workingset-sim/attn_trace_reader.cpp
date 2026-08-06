#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <stdexcept>

#include "attn_trace_reader.h"
#include "attn_workload.h"

namespace wssim
{

attn_trace_chunk_cache_t::attn_trace_chunk_cache_t(size_t budget_bytes)
	: m_budget_bytes(budget_bytes)
{
	m_stats.budget_bytes = budget_bytes;
}

void	attn_trace_chunk_cache_t::touch_lru_locked(uint32_t chunk_id,
				slot_t &slot)
{
	if (slot.in_lru)
		m_lru.erase(slot.lru_it);
	m_lru.push_front(chunk_id);
	slot.lru_it = m_lru.begin();
	slot.in_lru = true;
}

void	attn_trace_chunk_cache_t::evict_to_budget_locked()
{
	while (m_resident_bytes > m_budget_bytes)
	{
		bool	evicted_one = false;

		for (auto it = m_lru.rbegin(); it != m_lru.rend(); ++it)
		{
			auto	sit = m_slots.find(*it);

			if (sit == m_slots.end() || sit->second.loading
					|| sit->second.refcount != 0)
				continue ;
			m_resident_bytes -= sit->second.data.size()
				* sizeof(membrane_attntrace_entry_t);
			m_lru.erase(sit->second.lru_it);
			m_slots.erase(sit);
			m_stats.evictions++;
			evicted_one = true;
			break ;
		}
		if (!evicted_one)
			break ;
	}
}

const std::vector<membrane_attntrace_entry_t>	*attn_trace_chunk_cache_t::acquire(
	uint32_t chunk_id,
	const std::function<std::vector<membrane_attntrace_entry_t>()> &loader)
{
	std::unique_lock<std::mutex>	lock(m_mutex);

	for (;;)
	{
		auto	it = m_slots.find(chunk_id);

		if (it != m_slots.end() && !it->second.loading)
		{
			it->second.refcount++;
			touch_lru_locked(chunk_id, it->second);
			m_stats.hits++;
			return (&it->second.data);
		}
		if (it != m_slots.end() && it->second.loading)
		{
			m_stats.duplicate_loads_avoided++;
			m_cv.wait(lock, [&]
			{
				auto	w = m_slots.find(chunk_id);
				return (w == m_slots.end() || !w->second.loading);
			});
			continue ;	/* re-check: might now be a hit, or the loader
						 * may have failed and removed the slot,
						 * in which case we fall through to load it
						 * ourselves below. */
		}
		break ;
	}
	m_slots[chunk_id].loading = true;
	m_stats.misses++;
	lock.unlock();

	/* decode() (the loader) can throw on a corrupt/truncated chunk.
	 * Without this catch, the "loading" placeholder above is left
	 * forever (loading=true, no data) -- any OTHER thread already
	 * waiting in the m_cv.wait() predicate above, or one that arrives
	 * later, would block on it indefinitely, since nothing would ever
	 * flip `loading` back to false or erase the slot. Erase the
	 * placeholder and wake every waiter (who will then retry the load
	 * themselves, or hit the same real corruption and throw too) before
	 * propagating the failure to THIS caller. */
	std::vector<membrane_attntrace_entry_t>	decoded;
	try
	{
		decoded = loader();
	}
	catch (...)
	{
		lock.lock();
		m_slots.erase(chunk_id);
		m_cv.notify_all();
		throw ;
	}

	lock.lock();
	slot_t	&slot = m_slots[chunk_id];
	slot.data = std::move(decoded);
	slot.loading = false;
	slot.refcount = 1;
	m_resident_bytes += slot.data.size() * sizeof(membrane_attntrace_entry_t);
	touch_lru_locked(chunk_id, slot);
	evict_to_budget_locked();
	m_cv.notify_all();
	return (&slot.data);
}

void	attn_trace_chunk_cache_t::release(uint32_t chunk_id)
{
	std::lock_guard<std::mutex>	lock(m_mutex);
	auto							it = m_slots.find(chunk_id);

	if (it == m_slots.end())
		return ;
	if (it->second.refcount > 0)
		it->second.refcount--;
	evict_to_budget_locked();
}

void	attn_trace_chunk_cache_t::set_budget_bytes(size_t n)
{
	std::lock_guard<std::mutex>	lock(m_mutex);

	m_budget_bytes = n;
	m_stats.budget_bytes = n;
	evict_to_budget_locked();
}

attn_trace_chunk_cache_t::stats_t	attn_trace_chunk_cache_t::stats() const
{
	std::lock_guard<std::mutex>	lock(m_mutex);
	stats_t							s = m_stats;

	s.bytes_resident = m_resident_bytes;
	return (s);
}

namespace
{

trace_metadata_t	metadata_from_header(const membrane_attntrace3_header_t &h)
{
	trace_metadata_t	m;

	m.model = h.model;
	m.is_real_capture = (h.source == MEMBRANE_ATTNTRACE_SOURCE_REAL_CAPTURE);
	m.n_layer = h.n_layer;
	m.n_head = h.n_head;
	m.block_size_tokens = h.block_size_tokens;
	m.prompt_len = h.prompt_len;
	m.step_count = h.step_count;
	m.top_k = h.top_k;
	return (m);
}

/*
 * Shared sequential-access bookkeeping (current auto-pinned chunk +
 * an independent explicit prefetch-pin set) used identically by the
 * mmap and buffered-streaming backends -- only how raw chunk bytes
 * are obtained differs between them (see decode_from_mmap/decode_via_
 * file below), everything about chunk lifetime is common.
 */
class chunked_reader_base_t : public attn_trace_reader_t
{
public:
	chunked_reader_base_t(std::shared_ptr<attn_trace_chunk_cache_t> cache,
			size_t private_budget_bytes)
	{
		m_cache = cache != nullptr ? cache
			: std::make_shared<attn_trace_chunk_cache_t>(private_budget_bytes);
	}

	const trace_metadata_t	&get_metadata() const override { return (m_meta); }

	const membrane_attntrace_entry_t	*at(uint32_t step, uint32_t layer,
						uint32_t head) override
	{
		uint32_t	chunk_id = membrane_attntrace3_step_to_chunk(&m_hdr, step);

		if (chunk_id != m_cur_chunk_id)
		{
			if (m_cur_chunk_id != UINT32_MAX)
			{
				m_cache->release(m_cur_chunk_id);
				/* Invalidate BEFORE acquire(), not after: decode()
				 * (called by acquire()'s loader on a cache miss) can
				 * throw on a corrupt/truncated chunk. If that happens
				 * with the old id still in m_cur_chunk_id, a LATER
				 * at() call on this same reader instance (it's reused
				 * across every scenario a worker thread processes)
				 * could see "chunk_id == m_cur_chunk_id", skip
				 * re-acquiring, and dereference m_cur_ptr into a
				 * chunk this reader no longer holds a pin on -- a
				 * real dangling-pointer risk once the cache evicts
				 * it. Sentinel-ing here means any subsequent call
				 * always re-acquires fresh, regardless of whether the
				 * caller catches and retries with this same reader. */
				m_cur_chunk_id = UINT32_MAX;
				m_cur_ptr = nullptr;
			}
			m_cur_ptr = m_cache->acquire(chunk_id,
				[this, chunk_id] { return (decode(chunk_id)); });
			m_cur_chunk_id = chunk_id;
			readahead_from(chunk_id);
		}
		uint32_t	step_in_chunk = step - m_index[chunk_id].step_lo;
		size_t		base = (((size_t)step_in_chunk * m_hdr.n_layer + layer)
				* m_hdr.n_head + head) * m_hdr.top_k;
		return (&(*m_cur_ptr)[base]);
	}

	void	prefetch_chunk(uint32_t step) override
	{
		prefetch_chunk_id(membrane_attntrace3_step_to_chunk(&m_hdr, step));
	}

	void	release_chunk(uint32_t step) override
	{
		release_chunk_id(membrane_attntrace3_step_to_chunk(&m_hdr, step));
	}

	/* Phase 6.5 item 3's --prefetch-depth: on every chunk-boundary
	 * crossing (see readahead_from below, called from at()), warm the
	 * next `depth` chunks into the shared cache ahead of when the
	 * strictly-sequential calibration loop will actually need them,
	 * and drop any previously-readahead chunk that's now behind the
	 * current position. 0 (default) disables this -- at() then only
	 * ever holds its own single current-chunk pin, matching pre-6.5
	 * behavior exactly. */
	void	set_prefetch_depth(uint32_t depth) override
	{
		m_prefetch_depth = depth;
	}

	void	close() override
	{
		if (m_cur_chunk_id != UINT32_MAX)
		{
			m_cache->release(m_cur_chunk_id);
			m_cur_chunk_id = UINT32_MAX;
		}
		for (uint32_t cid : m_prefetched)
			m_cache->release(cid);
		m_prefetched.clear();
	}

protected:
	bool	open_header_and_index(FILE *f)
	{
		if (membrane_attntrace3_read_header(f, &m_hdr) != MEMBRANE_OK)
			return (false);
		m_index.resize(m_hdr.chunk_count);
		if (membrane_attntrace3_read_index(f, &m_hdr, m_index.data())
				!= MEMBRANE_OK)
			return (false);
		m_meta = metadata_from_header(m_hdr);
		return (true);
	}

	virtual std::vector<membrane_attntrace_entry_t>	decode(
						uint32_t chunk_id) = 0;

	membrane_attntrace3_header_t							m_hdr{};
	std::vector<membrane_attntrace3_chunk_index_entry_t>	m_index;
	trace_metadata_t										m_meta{};
	uint32_t												m_cur_chunk_id
		= UINT32_MAX;
	const std::vector<membrane_attntrace_entry_t>			*m_cur_ptr
		= nullptr;
	std::unordered_set<uint32_t>							m_prefetched;
	uint32_t												m_prefetch_depth
		= 0;

private:
	void	prefetch_chunk_id(uint32_t cid)
	{
		if (cid >= m_hdr.chunk_count || m_prefetched.count(cid))
			return ;
		m_cache->acquire(cid, [this, cid] { return (decode(cid)); });
		m_prefetched.insert(cid);
	}

	void	release_chunk_id(uint32_t cid)
	{
		auto	it = m_prefetched.find(cid);

		if (it != m_prefetched.end())
		{
			m_cache->release(cid);
			m_prefetched.erase(it);
		}
	}

	/* Called whenever at() crosses into a new current chunk: warms
	 * [chunk_id+1, chunk_id+m_prefetch_depth] and drops any
	 * previously-readahead chunk that fell behind chunk_id (the
	 * calibration loop's access is strictly forward, so anything
	 * behind is never needed again by THIS reader instance). */
	void	readahead_from(uint32_t chunk_id)
	{
		if (m_prefetch_depth == 0)
			return ;
		for (auto it = m_prefetched.begin(); it != m_prefetched.end(); )
		{
			if (*it < chunk_id)
			{
				m_cache->release(*it);
				it = m_prefetched.erase(it);
			}
			else
				++it;
		}
		for (uint32_t d = 1; d <= m_prefetch_depth; d++)
			prefetch_chunk_id(chunk_id + d);
	}
};

class mmap_reader_t final : public chunked_reader_base_t
{
public:
	using chunked_reader_base_t::chunked_reader_base_t;

	~mmap_reader_t() override { mmap_reader_t::close(); }

	bool	open(const std::string &path) override
	{
		struct stat	st;

		m_file = fopen(path.c_str(), "rb");
		if (m_file == nullptr)
			return (false);
		if (!open_header_and_index(m_file))
		{
			fclose(m_file);
			m_file = nullptr;
			return (false);
		}
		if (fstat(fileno(m_file), &st) != 0)
		{
			fclose(m_file);
			m_file = nullptr;
			return (false);
		}
		m_map_size = (size_t)st.st_size;
		m_map = mmap(nullptr, m_map_size, PROT_READ, MAP_PRIVATE,
			fileno(m_file), 0);
		if (m_map == MAP_FAILED)
		{
			m_map = nullptr;
			fclose(m_file);
			m_file = nullptr;
			return (false);
		}
		return (true);
	}

	void	close() override
	{
		chunked_reader_base_t::close();
		if (m_map != nullptr)
		{
			munmap(m_map, m_map_size);
			m_map = nullptr;
		}
		if (m_file != nullptr)
		{
			fclose(m_file);
			m_file = nullptr;
		}
	}

protected:
	std::vector<membrane_attntrace_entry_t>	decode(
				uint32_t chunk_id) override
	{
		const membrane_attntrace3_chunk_index_entry_t	&e = m_index[chunk_id];
		std::vector<membrane_attntrace_entry_t>			out(
			membrane_attntrace3_chunk_entry_count(&m_hdr, &e));
		const uint8_t	*stored = (const uint8_t *)m_map + e.payload_offset;

		if (membrane_attntrace3_decode_chunk_buf(stored, &e, out.data())
				!= MEMBRANE_OK)
			throw std::runtime_error(
				"attn_trace_reader (mmap): corrupt chunk " +
				std::to_string(chunk_id));
		return (out);
	}

private:
	FILE	*m_file = nullptr;
	void	*m_map = nullptr;
	size_t	m_map_size = 0;
};

class buffered_streaming_reader_t final : public chunked_reader_base_t
{
public:
	using chunked_reader_base_t::chunked_reader_base_t;

	~buffered_streaming_reader_t() override
	{
		buffered_streaming_reader_t::close();
	}

	bool	open(const std::string &path) override
	{
		m_file = fopen(path.c_str(), "rb");
		if (m_file == nullptr)
			return (false);
		if (!open_header_and_index(m_file))
		{
			fclose(m_file);
			m_file = nullptr;
			return (false);
		}
		return (true);
	}

	void	close() override
	{
		chunked_reader_base_t::close();
		if (m_file != nullptr)
		{
			fclose(m_file);
			m_file = nullptr;
		}
	}

protected:
	std::vector<membrane_attntrace_entry_t>	decode(
				uint32_t chunk_id) override
	{
		const membrane_attntrace3_chunk_index_entry_t	&e = m_index[chunk_id];
		std::vector<membrane_attntrace_entry_t>			out(
			membrane_attntrace3_chunk_entry_count(&m_hdr, &e));

		if (membrane_attntrace3_read_chunk(m_file, &m_hdr, &e, out.data())
				!= MEMBRANE_OK)
			throw std::runtime_error(
				"attn_trace_reader (streaming): corrupt chunk " +
				std::to_string(chunk_id));
		return (out);
	}

private:
	FILE	*m_file = nullptr;
};

class in_memory_reader_t final : public attn_trace_reader_t
{
public:
	explicit in_memory_reader_t(const attn_trace_t &trace) : m_trace(&trace)
	{
		m_meta.model = trace.model;
		m_meta.is_real_capture = trace.is_real_capture;
		m_meta.n_layer = trace.n_layer;
		m_meta.n_head = trace.n_head;
		m_meta.block_size_tokens = trace.block_size_tokens;
		m_meta.prompt_len = trace.prompt_len;
		m_meta.step_count = trace.step_count;
		m_meta.top_k = trace.top_k;
	}

	/* No path to open -- the trace is already fully resident, supplied
	 * at construction (see make_in_memory_reader). */
	bool	open(const std::string &path) override
	{
		(void)path;
		return (m_trace != nullptr);
	}

	const trace_metadata_t	&get_metadata() const override { return (m_meta); }

	const membrane_attntrace_entry_t	*at(uint32_t step, uint32_t layer,
						uint32_t head) override
	{
		return (m_trace->at(step, layer, head));
	}

	void	close() override {}

private:
	const attn_trace_t	*m_trace;
	trace_metadata_t	m_meta;
};

}	/* anonymous namespace */

std::unique_ptr<attn_trace_reader_t>	make_attn_trace_reader(
	trace_backend_t backend, std::shared_ptr<attn_trace_chunk_cache_t> cache,
	size_t private_cache_budget_bytes)
{
	if (backend == trace_backend_t::MMAP)
		return (std::make_unique<mmap_reader_t>(cache,
			private_cache_budget_bytes));
	if (backend == trace_backend_t::STREAMING)
		return (std::make_unique<buffered_streaming_reader_t>(cache,
			private_cache_budget_bytes));
	return (nullptr);	/* IN_MEMORY has no file to open -- use
						 * make_in_memory_reader() directly. */
}

std::unique_ptr<attn_trace_reader_t>	make_in_memory_reader(
	const attn_trace_t &trace)
{
	return (std::make_unique<in_memory_reader_t>(trace));
}

}	/* namespace wssim */
