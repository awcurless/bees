#include "bees.h"

#include "crucible/crc64.h"
#include "crucible/string.h"

#include <algorithm>
#include <random>

#include <sys/mman.h>

using namespace crucible;
using namespace std;

ostream &
operator<<(ostream &os, const BeesHash &bh)
{
	return os << to_hex(BeesHash::Type(bh));
}

ostream &
operator<<(ostream &os, const BeesHashTable::Cell &bhte)
{
	return os << "BeesHashTable::Cell { hash = " << BeesHash(bhte.e_hash) << ", addr = "
		  << BeesAddress(bhte.e_addr) << " }";
}

void
dump_bucket(BeesHashTable::Cell *p, BeesHashTable::Cell *q)
{
	// Must be called while holding m_bucket_mutex
	for (auto i = p; i < q; ++i) {
		BEESLOG("Entry " << i - p << " " << *i);
	}
}

const bool VERIFY_CLEARS_BUGS = false;

bool
verify_cell_range(BeesHashTable::Cell *p, BeesHashTable::Cell *q, bool clear_bugs = VERIFY_CLEARS_BUGS)
{
	// Must be called while holding m_bucket_mutex
	bool bugs_found = false;
	set<BeesHashTable::Cell> seen_it;
	for (BeesHashTable::Cell *cell = p; cell < q; ++cell) {
		if (cell->e_addr && cell->e_addr < 0x1000) {
			BEESCOUNT(bug_hash_magic_addr);
			BEESINFO("Bad hash table address hash " << to_hex(cell->e_hash) << " addr " << to_hex(cell->e_addr));
			if (clear_bugs) {
				cell->e_addr = 0;
				cell->e_hash = 0;
			}
			bugs_found = true;
		}
		if (cell->e_addr && !seen_it.insert(*cell).second) {
			BEESCOUNT(bug_hash_duplicate_cell);
			// BEESLOG("Duplicate hash table entry:\nthis = " << *cell << "\nold = " << *seen_it.find(*cell));
			BEESINFO("Duplicate hash table entry: " << *cell);
			if (clear_bugs) {
				cell->e_addr = 0;
				cell->e_hash = 0;
			}
			bugs_found = true;
		}
	}
	return bugs_found;
}

pair<BeesHashTable::Cell *, BeesHashTable::Cell *>
BeesHashTable::get_cell_range(HashType hash)
{
	THROW_CHECK1(runtime_error, m_buckets, m_buckets > 0);
	THROW_CHECK1(runtime_error, m_bucket_ptr, m_bucket_ptr != nullptr);
	Bucket *pp = &m_bucket_ptr[hash % m_buckets];
	Cell *bp = pp[0].p_cells;
	Cell *ep = pp[1].p_cells;
	THROW_CHECK2(out_of_range, m_cell_ptr,     bp, bp >= m_cell_ptr);
	THROW_CHECK2(out_of_range, m_cell_ptr_end, ep, ep <= m_cell_ptr_end);
	return make_pair(bp, ep);
}

pair<uint8_t *, uint8_t *>
BeesHashTable::get_extent_range(HashType hash)
{
	THROW_CHECK1(runtime_error, m_buckets, m_buckets > 0);
	THROW_CHECK1(runtime_error, m_bucket_ptr, m_bucket_ptr != nullptr);
	Extent *iop = &m_extent_ptr[ (hash % m_buckets) / c_buckets_per_extent ];
	uint8_t *bp = iop[0].p_byte;
	uint8_t *ep = iop[1].p_byte;
	THROW_CHECK2(out_of_range, m_byte_ptr,     bp, bp >= m_byte_ptr);
	THROW_CHECK2(out_of_range, m_byte_ptr_end, ep, ep <= m_byte_ptr_end);
	return make_pair(bp, ep);
}

void
BeesHashTable::flush_dirty_extents()
{
	THROW_CHECK1(runtime_error, m_buckets, m_buckets > 0);

	unique_lock<mutex> lock(m_extent_mutex);
	auto dirty_extent_copy = m_buckets_dirty;
	m_buckets_dirty.clear();
	if (dirty_extent_copy.empty()) {
		BEESNOTE("idle");
		m_condvar.wait(lock);
		return; // please call later, i.e. immediately
	}
	lock.unlock();

	size_t extent_counter = 0;
	for (auto extent_number : dirty_extent_copy) {
		++extent_counter;
		BEESNOTE("flush extent #" << extent_number << " (" << extent_counter << " of " << dirty_extent_copy.size() << ")");
		catch_all([&]() {
			uint8_t *dirty_extent     = m_extent_ptr[extent_number].p_byte;
			uint8_t *dirty_extent_end = m_extent_ptr[extent_number + 1].p_byte;
			THROW_CHECK1(out_of_range, dirty_extent,     dirty_extent     >= m_byte_ptr);
			THROW_CHECK1(out_of_range, dirty_extent_end, dirty_extent_end <= m_byte_ptr_end);
			THROW_CHECK2(out_of_range, dirty_extent_end, dirty_extent, dirty_extent_end - dirty_extent == BLOCK_SIZE_HASHTAB_EXTENT);
			BEESTOOLONG("pwrite(fd " << m_fd << " '" << name_fd(m_fd)<< "', length " << to_hex(dirty_extent_end - dirty_extent) << ", offset " << to_hex(dirty_extent - m_byte_ptr) << ")");
			// Page locks slow us down more than copying the data does
			vector<uint8_t> extent_copy(dirty_extent, dirty_extent_end);
			pwrite_or_die(m_fd, extent_copy, dirty_extent - m_byte_ptr);
			BEESCOUNT(hash_extent_out);
		});
		BEESNOTE("flush rate limited at extent #" << extent_number << " (" << extent_counter << " of " << dirty_extent_copy.size() << ")");
		m_flush_rate_limit.sleep_for(BLOCK_SIZE_HASHTAB_EXTENT);
	}
}

void
BeesHashTable::set_extent_dirty(HashType hash)
{
	THROW_CHECK1(runtime_error, m_buckets, m_buckets > 0);
	auto pr = get_extent_range(hash);
	uint64_t extent_number = reinterpret_cast<Extent *>(pr.first) - m_extent_ptr;
	THROW_CHECK1(runtime_error, extent_number, extent_number < m_extents);
	unique_lock<mutex> lock(m_extent_mutex);
	m_buckets_dirty.insert(extent_number);
	m_condvar.notify_one();
}

void
BeesHashTable::writeback_loop()
{
	while (true) {
		flush_dirty_extents();
	}
}

static
string
percent(size_t num, size_t den)
{
	if (den) {
		return astringprintf("%u%%", num * 100 / den);
	} else {
		return "--%";
	}
}

void
BeesHashTable::prefetch_loop()
{
	// Always do the mlock, whether shared or not
	THROW_CHECK1(runtime_error, m_size, m_size > 0);
	catch_all([&]() {
		BEESNOTE("mlock " << pretty(m_size));
		DIE_IF_NON_ZERO(mlock(m_byte_ptr, m_size));
	});

	while (1) {
		size_t width = 64;
		vector<size_t> occupancy(width, 0);
		size_t occupied_count = 0;
		size_t total_count = 0;
		size_t compressed_count = 0;
		size_t compressed_offset_count = 0;
		size_t toxic_count = 0;
		size_t unaligned_eof_count = 0;

		for (uint64_t ext = 0; ext < m_extents; ++ext) {
			BEESNOTE("prefetching hash table extent " << ext << " of " << m_extent_ptr_end - m_extent_ptr);
			catch_all([&]() {
				fetch_missing_extent(ext * c_buckets_per_extent);

				BEESNOTE("analyzing hash table extent " << ext << " of " << m_extent_ptr_end - m_extent_ptr);
				bool duplicate_bugs_found = false;
				unique_lock<mutex> lock(m_bucket_mutex);
				for (Bucket *bucket = m_extent_ptr[ext].p_buckets; bucket < m_extent_ptr[ext + 1].p_buckets; ++bucket) {
					if (verify_cell_range(bucket[0].p_cells, bucket[1].p_cells)) {
						duplicate_bugs_found = true;
					}
					size_t this_bucket_occupied_count = 0;
					for (Cell *cell = bucket[0].p_cells; cell < bucket[1].p_cells; ++cell) {
						if (cell->e_addr) {
							++this_bucket_occupied_count;
							BeesAddress a(cell->e_addr);
							if (a.is_compressed()) {
								++compressed_count;
								if (a.has_compressed_offset()) {
									++compressed_offset_count;
								}
							}
							if (a.is_toxic()) {
								++toxic_count;
							}
							if (a.is_unaligned_eof()) {
								++unaligned_eof_count;
							}
						}
						++total_count;
					}
					++occupancy.at(this_bucket_occupied_count * width / (1 + c_cells_per_bucket) );
					// Count these instead of calculating the number so we get better stats in case of exceptions
					occupied_count += this_bucket_occupied_count;
				}
				lock.unlock();
				if (duplicate_bugs_found) {
					set_extent_dirty(ext);
				}
			});
		}

		BEESNOTE("calculating hash table statistics");

		vector<string> histogram;
		vector<size_t> thresholds;
		size_t threshold = 1;
		bool threshold_exceeded = false;
		do {
			threshold_exceeded = false;
			histogram.push_back(string(width, ' '));
			thresholds.push_back(threshold);
			for (size_t x = 0; x < width; ++x) {
				if (occupancy.at(x) >= threshold) {
					histogram.back().at(x) = '#';
					threshold_exceeded = true;
				}
			}
			threshold *= 2;
		} while (threshold_exceeded);

		ostringstream out;
		size_t count = histogram.size();
		bool first_line = true;
		for (auto it = histogram.rbegin(); it != histogram.rend(); ++it) {
			out << *it << " " << thresholds.at(--count);
			if (first_line) {
				first_line = false;
				out << " pages";
			}
			out << "\n";
		}

		size_t uncompressed_count = occupied_count - compressed_count;
		size_t legacy_count = compressed_count - compressed_offset_count;

		ostringstream graph_blob;

		graph_blob << "Now:     " << format_time(time(NULL)) << "\n";
		graph_blob << "Uptime:  " << m_ctx->total_timer().age() << " seconds\n";
		graph_blob << "Version: " << BEES_VERSION << "\n";

		graph_blob
			<< "\nHash table page occupancy histogram (" << occupied_count << "/" << total_count << " cells occupied, " << (occupied_count * 100 / total_count) << "%)\n"
			<< out.str() << "0%      |      25%      |      50%      |      75%      |   100% page fill\n"
			<< "compressed " << compressed_count << " (" << percent(compressed_count, occupied_count) << ")"
			<< " new-style " << compressed_offset_count << " (" << percent(compressed_offset_count, occupied_count) << ")"
			<< " old-style " << legacy_count << " (" << percent(legacy_count, occupied_count) << ")\n"
			<< "uncompressed " << uncompressed_count << " (" << percent(uncompressed_count, occupied_count) << ")"
			<< " unaligned_eof " << unaligned_eof_count << " (" << percent(unaligned_eof_count, occupied_count) << ")"
			<< " toxic " << toxic_count << " (" << percent(toxic_count, occupied_count) << ")";

		graph_blob << "\n\n";

		graph_blob << "TOTAL:\n";
		auto thisStats = BeesStats::s_global;
		graph_blob << "\t" << thisStats << "\n";

		graph_blob << "\nRATES:\n";
		auto avg_rates = thisStats / m_ctx->total_timer().age();
		graph_blob << "\t" << avg_rates << "\n";

		BEESLOG(graph_blob.str());
		catch_all([&]() {
			m_stats_file.write(graph_blob.str());
		});

		BEESNOTE("idle " << BEES_HASH_TABLE_ANALYZE_INTERVAL << "s");
		nanosleep(BEES_HASH_TABLE_ANALYZE_INTERVAL);
	}
}

void
BeesHashTable::fetch_missing_extent(HashType hash)
{
	BEESTOOLONG("fetch_missing_extent for hash " << to_hex(hash));
	THROW_CHECK1(runtime_error, m_buckets, m_buckets > 0);
	auto pr = get_extent_range(hash);
	uint64_t extent_number = reinterpret_cast<Extent *>(pr.first) - m_extent_ptr;
	THROW_CHECK1(runtime_error, extent_number, extent_number < m_extents);

	unique_lock<mutex> lock(m_extent_mutex);
	if (!m_buckets_missing.count(extent_number)) {
		return;
	}

	size_t missing_buckets = m_buckets_missing.size();
	lock.unlock();

	BEESNOTE("waiting to fetch hash extent #" << extent_number << ", " << missing_buckets << " left to fetch");

	// Acquire blocking lock on this extent only
	LockSet<uint64_t>::Lock extent_lock(m_extent_lock_set, extent_number);

	// Check missing again because someone else might have fetched this
	// extent for us while we didn't hold any locks
	lock.lock();
	if (!m_buckets_missing.count(extent_number)) {
		BEESCOUNT(hash_extent_in_twice);
		return;
	}
	lock.unlock();

	// OK we have to read this extent
	BEESNOTE("fetching hash extent #" << extent_number << ", " << missing_buckets << " left to fetch");

	BEESTRACE("Fetching missing hash extent " << extent_number);
	uint8_t *dirty_extent     = m_extent_ptr[extent_number].p_byte;
	uint8_t *dirty_extent_end = m_extent_ptr[extent_number + 1].p_byte;

	{
		BEESTOOLONG("pread(fd " << m_fd << " '" << name_fd(m_fd)<< "', length " << to_hex(dirty_extent_end - dirty_extent) << ", offset " << to_hex(dirty_extent - m_byte_ptr) << ")");
		pread_or_die(m_fd, dirty_extent, dirty_extent_end - dirty_extent, dirty_extent - m_byte_ptr);
	}

	BEESCOUNT(hash_extent_in);
	// We don't block when fetching an extent but we do slow down the
	// prefetch thread.
	m_prefetch_rate_limit.borrow(BLOCK_SIZE_HASHTAB_EXTENT);
	lock.lock();
	m_buckets_missing.erase(extent_number);
}

bool
BeesHashTable::is_toxic_hash(BeesHashTable::HashType hash) const
{
	return m_toxic_hashes.find(hash) != m_toxic_hashes.end();
}

vector<BeesHashTable::Cell>
BeesHashTable::find_cell(HashType hash)
{
	// This saves a lot of time prefilling the hash table, and there's no risk of eviction
	if (is_toxic_hash(hash)) {
		BEESCOUNT(hash_toxic);
		BeesAddress toxic_addr(0x1000);
		toxic_addr.set_toxic();
		Cell toxic_cell(hash, toxic_addr);
		vector<Cell> rv;
		rv.push_back(toxic_cell);
		return rv;
	}
	fetch_missing_extent(hash);
	BEESTOOLONG("find_cell hash " << BeesHash(hash));
	vector<Cell> rv;
	unique_lock<mutex> lock(m_bucket_mutex);
	auto er = get_cell_range(hash);
	// FIXME:  Weed out zero addresses in the table due to earlier bugs
	copy_if(er.first, er.second, back_inserter(rv), [=](const Cell &ip) { return ip.e_hash == hash && ip.e_addr >= 0x1000; });
	BEESCOUNT(hash_lookup);
	return rv;
}

// Move an entry to the end of the list.  Used after an attempt to resolve
// an address in the hash table fails.  Probably more correctly called
// push_back_hash_addr, except it never inserts.  Shared hash tables
// never erase anything, since there is no way to tell if an entry is
// out of date or just belonging to the wrong filesystem.
void
BeesHashTable::erase_hash_addr(HashType hash, AddrType addr)
{
	fetch_missing_extent(hash);
	BEESTOOLONG("erase hash " << to_hex(hash) << " addr " << addr);
	unique_lock<mutex> lock(m_bucket_mutex);
	auto er = get_cell_range(hash);
	Cell mv(hash, addr);
	Cell *ip = find(er.first, er.second, mv);
	bool found = (ip < er.second);
	if (found) {
		// Lookups on invalid addresses really hurt us.  Kill it with fire!
		*ip = Cell(0, 0);
		set_extent_dirty(hash);
		BEESCOUNT(hash_erase);
#if 0
		if (verify_cell_range(er.first, er.second)) {
			BEESINFO("while erasing hash " << hash << " addr " << addr);
		}
#endif
	}
}

// If entry is already present in list, move it to the front of the
// list without dropping any entries, and return true.  If entry is not
// present in list, insert it at the front of the list, possibly dropping
// the last entry in the list, and return false.  Used to move duplicate
// hash blocks to the front of the list.
bool
BeesHashTable::push_front_hash_addr(HashType hash, AddrType addr)
{
	fetch_missing_extent(hash);
	BEESTOOLONG("push_front_hash_addr hash " << BeesHash(hash) <<" addr " << BeesAddress(addr));
	unique_lock<mutex> lock(m_bucket_mutex);
	auto er = get_cell_range(hash);
	Cell mv(hash, addr);
	Cell *ip = find(er.first, er.second, mv);
	bool found = (ip < er.second);
	if (!found) {
		// If no match found, get rid of an empty space instead
		// If no empty spaces, ip will point to end
		ip = find(er.first, er.second, Cell(0, 0));
	}
	if (ip > er.first) {
		// Delete matching entry, first empty entry,
		// or last entry whether empty or not
		// move_backward(er.first, ip - 1, ip);
		auto sp = ip;
		auto dp = ip;
		--sp;
		// If we are deleting the last entry then don't copy it
		if (ip == er.second) {
			--sp;
			--dp;
			BEESCOUNT(hash_evict);
		}
		while (dp > er.first) {
			*dp-- = *sp--;
		}
	}
	// There is now a space at the front, insert there if different
	if (er.first[0] != mv) {
		er.first[0] = mv;
		set_extent_dirty(hash);
		BEESCOUNT(hash_front);
	}
#if 0
	if (verify_cell_range(er.first, er.second)) {
		BEESINFO("while push_fronting hash " << hash << " addr " << addr);
	}
#endif
	return found;
}

// If entry is already present in list, returns true and does not
// modify list.  If entry is not present in list, returns false and
// inserts at a random position in the list, possibly evicting the entry
// at the end of the list.  Used to insert new unique (not-yet-duplicate)
// blocks in random order.
bool
BeesHashTable::push_random_hash_addr(HashType hash, AddrType addr)
{
	fetch_missing_extent(hash);
	BEESTOOLONG("push_random_hash_addr hash " << BeesHash(hash) << " addr " << BeesAddress(addr));
	unique_lock<mutex> lock(m_bucket_mutex);
	auto er = get_cell_range(hash);
	Cell mv(hash, addr);
	Cell *ip = find(er.first, er.second, mv);
	bool found = (ip < er.second);

	thread_local default_random_engine generator;
	thread_local uniform_int_distribution<int> distribution(0, c_cells_per_bucket - 1);
	auto pos = distribution(generator);

	int case_cond = 0;
	vector<Cell> saved(er.first, er.second);

	if (found) {
		// If hash already exists after pos, swap with pos
		if (ip > er.first + pos) {

			// move_backward(er.first + pos, ip - 1, ip);
			auto sp = ip;
			auto dp = ip;
			--sp;
			while (dp > er.first + pos) {
				*dp-- = *sp--;
			}
			*dp = mv;
			BEESCOUNT(hash_bump);
			case_cond = 1;
			goto ret_dirty;
		}
		// Hash already exists before (or at) pos, leave it there
		BEESCOUNT(hash_already);
		case_cond = 2;
		goto ret;
	}

	// Find an empty space to back of pos
	for (ip = er.first + pos; ip < er.second; ++ip) {
		if (*ip == Cell(0, 0)) {
			*ip = mv;
			case_cond = 3;
			goto ret_dirty;
		}
	}

	// Find an empty space to front of pos
	// if there is anything to front of pos
	if (pos > 0) {
		for (ip = er.first + pos - 1; ip >= er.first; --ip) {
			if (*ip == Cell(0, 0)) {
				*ip = mv;
				case_cond = 4;
				goto ret_dirty;
			}
		}
	}

	// Evict something and insert at pos
	move_backward(er.first + pos, er.second - 1, er.second);
	er.first[pos] = mv;
	BEESCOUNT(hash_evict);
	case_cond = 5;
ret_dirty:
	BEESCOUNT(hash_insert);
	set_extent_dirty(hash);
ret:
#if 0
	if (verify_cell_range(er.first, er.second, false)) {
		BEESLOG("while push_randoming (case " << case_cond << ") pos " << pos
			<< " ip " << (ip - er.first) << " " << mv);
		// dump_bucket(saved.data(), saved.data() + saved.size());
		// dump_bucket(er.first, er.second);
	}
#else
	(void)case_cond;
#endif
	return found;
}

void
BeesHashTable::try_mmap_flags(int flags)
{
	if (!m_cell_ptr) {
		THROW_CHECK1(out_of_range, m_size, m_size > 0);
		Timer map_time;
		catch_all([&]() {
			BEESLOG("mapping hash table size " << m_size << " with flags " << mmap_flags_ntoa(flags));
			void *ptr = mmap_or_die(nullptr, m_size, PROT_READ | PROT_WRITE, flags, flags & MAP_ANONYMOUS ? -1 : int(m_fd), 0);
			BEESLOG("mmap done in " << map_time << " sec");
			m_cell_ptr = static_cast<Cell *>(ptr);
			void *ptr_end = static_cast<uint8_t *>(ptr) + m_size;
			m_cell_ptr_end = static_cast<Cell *>(ptr_end);
		});
	}
}

void
BeesHashTable::open_file()
{
	// OK open hash table
	BEESNOTE("opening hash table '" << m_filename << "' target size " << m_size << " (" << pretty(m_size) << ")");

	// Try to open existing hash table
	Fd new_fd = openat(m_ctx->home_fd(), m_filename.c_str(), FLAGS_OPEN_FILE_RW, 0700);

	// If that doesn't work, try to make a new one
	if (!new_fd) {
		string tmp_filename = m_filename + ".tmp";
		BEESLOGNOTE("creating new hash table '" << tmp_filename << "'");
		unlinkat(m_ctx->home_fd(), tmp_filename.c_str(), 0);
		new_fd = openat_or_die(m_ctx->home_fd(), tmp_filename, FLAGS_CREATE_FILE, 0700);
		BEESLOGNOTE("truncating new hash table '" << tmp_filename << "' size " << m_size << " (" << pretty(m_size) << ")");
		ftruncate_or_die(new_fd, m_size);
		BEESLOGNOTE("truncating new hash table '" << tmp_filename << "' -> '" << m_filename << "'");
		renameat_or_die(m_ctx->home_fd(), tmp_filename, m_ctx->home_fd(), m_filename);
	}

	Stat st(new_fd);
	off_t new_size = st.st_size;

	THROW_CHECK1(invalid_argument, new_size, new_size > 0);
	THROW_CHECK1(invalid_argument, new_size, (new_size % BLOCK_SIZE_HASHTAB_EXTENT) == 0);
	m_size = new_size;
	m_fd = new_fd;
}

BeesHashTable::BeesHashTable(shared_ptr<BeesContext> ctx, string filename, off_t size) :
	m_ctx(ctx),
	m_size(0),
	m_void_ptr(nullptr),
	m_void_ptr_end(nullptr),
	m_buckets(0),
	m_cells(0),
	m_writeback_thread("hash_writeback"),
	m_prefetch_thread("hash_prefetch"),
	m_flush_rate_limit(BEES_FLUSH_RATE),
	m_prefetch_rate_limit(BEES_FLUSH_RATE),
	m_stats_file(m_ctx->home_fd(), "beesstats.txt")
{
	// Sanity checks to protect the implementation from its weaknesses
	THROW_CHECK2(invalid_argument, BLOCK_SIZE_HASHTAB_BUCKET, BLOCK_SIZE_HASHTAB_EXTENT, (BLOCK_SIZE_HASHTAB_EXTENT % BLOCK_SIZE_HASHTAB_BUCKET) == 0);

	// There's more than one union
	THROW_CHECK2(runtime_error, sizeof(Bucket), BLOCK_SIZE_HASHTAB_BUCKET, BLOCK_SIZE_HASHTAB_BUCKET == sizeof(Bucket));
	THROW_CHECK2(runtime_error, sizeof(Bucket::p_byte), BLOCK_SIZE_HASHTAB_BUCKET, BLOCK_SIZE_HASHTAB_BUCKET == sizeof(Bucket::p_byte));
	THROW_CHECK2(runtime_error, sizeof(Extent), BLOCK_SIZE_HASHTAB_EXTENT, BLOCK_SIZE_HASHTAB_EXTENT == sizeof(Extent));
	THROW_CHECK2(runtime_error, sizeof(Extent::p_byte), BLOCK_SIZE_HASHTAB_EXTENT, BLOCK_SIZE_HASHTAB_EXTENT == sizeof(Extent::p_byte));

	m_filename = filename;
	m_size = size;
	open_file();

	// Now we know size we can compute stuff

	BEESTRACE("hash table size " << m_size);
	BEESTRACE("hash table bucket size " << BLOCK_SIZE_HASHTAB_BUCKET);
	BEESTRACE("hash table extent size " << BLOCK_SIZE_HASHTAB_EXTENT);

	BEESLOG("opened hash table filename '" << filename << "' length " << m_size);
	m_buckets = m_size / BLOCK_SIZE_HASHTAB_BUCKET;
	m_cells = m_buckets * c_cells_per_bucket;
	m_extents = (m_size + BLOCK_SIZE_HASHTAB_EXTENT - 1) / BLOCK_SIZE_HASHTAB_EXTENT;
	BEESLOG("\tcells " << m_cells << ", buckets " << m_buckets << ", extents " << m_extents);

	BEESLOG("\tflush rate limit " << BEES_FLUSH_RATE);

	// Try to mmap that much memory
	try_mmap_flags(MAP_PRIVATE | MAP_ANONYMOUS);

	if (!m_cell_ptr) {
		THROW_ERRNO("unable to mmap " << filename);
	}

	// Do unions work the way we think (and rely on)?
	THROW_CHECK2(runtime_error, m_void_ptr, m_cell_ptr, m_void_ptr == m_cell_ptr);
	THROW_CHECK2(runtime_error, m_void_ptr, m_byte_ptr, m_void_ptr == m_byte_ptr);
	THROW_CHECK2(runtime_error, m_void_ptr, m_bucket_ptr, m_void_ptr == m_bucket_ptr);
	THROW_CHECK2(runtime_error, m_void_ptr, m_extent_ptr, m_void_ptr == m_extent_ptr);

	{
		// It's OK if this fails (e.g. kernel not built with CONFIG_TRANSPARENT_HUGEPAGE)
		// We don't fork any more so DONTFORK isn't really needed
		BEESTOOLONG("madvise(MADV_HUGEPAGE | MADV_DONTFORK)");
		if (madvise(m_byte_ptr, m_size, MADV_HUGEPAGE | MADV_DONTFORK)) {
			BEESLOG("mostly harmless: madvise(MADV_HUGEPAGE | MADV_DONTFORK) failed: " << strerror(errno));
		}
	}

	for (uint64_t i = 0; i < m_size / sizeof(Extent); ++i) {
		m_buckets_missing.insert(i);
	}

	m_writeback_thread.exec([&]() {
		writeback_loop();
        });

	m_prefetch_thread.exec([&]() {
		prefetch_loop();
        });

	// Blacklist might fail if the hash table is not stored on a btrfs
	catch_all([&]() {
		m_ctx->blacklist_add(BeesFileId(m_fd));
	});

	// Skip zero because we already weed that out before it gets near a hash function
	for (unsigned i = 1; i < 256; ++i) {
		vector<uint8_t> v(BLOCK_SIZE_SUMS, i);
		HashType hash = Digest::CRC::crc64(v.data(), v.size());
		m_toxic_hashes.insert(hash);
	}
}

BeesHashTable::~BeesHashTable()
{
	if (m_cell_ptr && m_size) {
		flush_dirty_extents();
		catch_all([&]() {
			DIE_IF_NON_ZERO(munmap(m_cell_ptr, m_size));
			m_cell_ptr = nullptr;
			m_size = 0;
		});
	}
}

