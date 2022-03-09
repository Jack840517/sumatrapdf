/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

namespace dict {

struct HashTable;

// we are very generous with default initial size. It's a trade-off
// between memory used by hash table and how often we need to resize it.
// We allocate size*sizeof(ptr) which is 64k on 32-bit for 16k entries.
// 64k is very little on today's machines, especially for short-lived
// hash tables.
// Should use smaller values for long-lived hash tables, especially
// if there are many of them.
enum { DEFAULT_HASH_TABLE_INITIAL_SIZE = 16 * 1024 };

// a dictionary whose keys are char * strings and the values are integers
// note: StrToInt would be more natural name but it's re-#define'd in <shlwapi.h>
class MapStrToInt {
  public:
    PoolAllocator allocator;
    HashTable* h = nullptr;

    explicit MapStrToInt(size_t initialSize = DEFAULT_HASH_TABLE_INITIAL_SIZE);
    ~MapStrToInt();

    [[nodiscard]] size_t Count() const;

    bool Insert(const char* key, int val, int* existingValOut = nullptr, const char** existingKeyOut = nullptr);

    bool Remove(const char* key, int* removedValOut) const;
    bool Get(const char* key, int* valOut) const;
};

class MapWStrToInt {
  public:
    PoolAllocator allocator;
    HashTable* h = nullptr;

    explicit MapWStrToInt(size_t initialSize = DEFAULT_HASH_TABLE_INITIAL_SIZE);
    ~MapWStrToInt();

    [[nodiscard]] size_t Count() const;

    // if a key doesn't exist, inserts a key with a given value and return true
    // if a key exists, returns false and sets prevValOut to existing value
    bool Insert(const WCHAR* key, int val, int* prevValOut);
    bool Remove(const WCHAR* key, int* removedValOut) const;
    bool Get(const WCHAR* key, int* valOut) const;
};

} // namespace dict

class StringInterner {
    dict::MapStrToInt strToInt;
    Vec<const char*> intToStr;

  public:
    StringInterner() = default;

    int Intern(const char* s, bool* alreadyPresent = nullptr);
    [[nodiscard]] size_t StringsCount() const {
        return intToStr.size();
    }
    [[nodiscard]] const char* GetByIndex(size_t n) const {
        return intToStr.at(n);
    }

    int nInternCalls{0}; // so we know how effective interning is
};
