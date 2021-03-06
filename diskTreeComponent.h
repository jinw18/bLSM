/*
 * diskTreeComponent.h
 *
 * Copyright 2010-2012 Yahoo! Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *  Created on: Feb 18, 2010
 *      Author: sears
 */

#ifndef DISKTREECOMPONENT_H_
#define DISKTREECOMPONENT_H_

#include "dataPage.h"
#include "dataTuple.h"
#include "mergeStats.h"
#include <stasis/util/bloomFilter.h>
#include <stasis/util/crc32.h>

extern "C" {
  static uint64_t diskTreeComponent_hash_func_a(const char* a, int len) {
    return stasis_crc32(a,len,0xcafebabe);
  }

  static uint64_t diskTreeComponent_hash_func_b(const char* a, int len) {
    return stasis_crc32(a,len,0xdeadbeef);
  }
}
class diskTreeComponent {
 public:
  class internalNodes;
  class iterator;

  diskTreeComponent(int xid, pageid_t internal_region_size, pageid_t datapage_region_size, pageid_t datapage_size,
                    mergeStats* stats, uint64_t bloom_filter_size = 0) :
    ltree(new diskTreeComponent::internalNodes(xid, internal_region_size, datapage_region_size, datapage_size)),
    dp(0),
    datapage_size(datapage_size),
    stats(stats),
    bloom_filter(bloom_filter_size == 0
                ? 0
                : stasis_bloom_filter_create(diskTreeComponent_hash_func_a,
                                      diskTreeComponent_hash_func_b,
                                      bloom_filter_size, 0.01))  {
    if(bloom_filter) stasis_bloom_filter_print_stats(bloom_filter);
  }

  diskTreeComponent(int xid, recordid root, recordid internal_node_state,
                    recordid datapage_state, mergeStats* stats) :
    ltree(new diskTreeComponent::internalNodes(xid, root, internal_node_state, datapage_state)),
    dp(0),
    datapage_size(-1),
    stats(stats),
    bloom_filter(0) {}

  ~diskTreeComponent() {
    if(bloom_filter) stasis_bloom_filter_destroy(bloom_filter);
    delete dp;
    delete ltree;
  }

  recordid get_root_rid();
  recordid get_datapage_allocator_rid();
  recordid get_internal_node_allocator_rid();
  internalNodes * get_internal_nodes() { return ltree; }
  dataTuple* findTuple(int xid, dataTuple::key_t key, size_t keySize);
  int insertTuple(int xid, dataTuple *t);
  void writes_done();


  iterator * open_iterator(mergeManager * mgr = NULL, double target_size = 0, bool * flushing = NULL) {
    return new iterator(ltree, mgr, target_size, flushing);
  }
  iterator * open_iterator(dataTuple * key) {
    if(key != NULL) {
      return new iterator(ltree, key);
    } else {
      return new iterator(ltree);
    }
  }

  void force(int xid);
  void dealloc(int xid);
  void list_regions(int xid, pageid_t *internal_node_region_length, pageid_t *internal_node_region_count, pageid_t **internal_node_regions,
		    pageid_t *datapage_region_length, pageid_t *datapage_region_count, pageid_t **datapage_regions);

  void print_tree(int xid) {
    ltree->print_tree(xid);
  }

 private:
  dataPage* insertDataPage(int xid, dataTuple *tuple);

  internalNodes * ltree;
  dataPage* dp;
  pageid_t datapage_size;
  /*mergeManager::mergeStats*/ void *stats; // XXX hack to work around circular includes.

 public:
  class internalNodes{
  public:

    internalNodes(int xid, pageid_t internal_region_size, pageid_t datapage_region_size, pageid_t datapage_size);
    internalNodes(int xid, recordid root, recordid internal_node_state, recordid datapage_state);
    ~internalNodes();
    void print_tree(int xid);

    //returns the id of the data page that could contain the given key
    pageid_t findPage(int xid, const byte *key, size_t keySize);

    //appends a leaf page, val_page is the id of the leaf page
    recordid appendPage(int xid, const byte *key,size_t keySize, pageid_t val_page);

    inline regionAllocator* get_datapage_alloc() { return datapage_alloc; }
    inline regionAllocator* get_internal_node_alloc() { return internal_node_alloc; }
    const recordid &get_root_rec(){return root_rec;}

  private:
    recordid create(int xid);

    void writeNodeRecord(int xid, Page *p, recordid &rid,
                                const byte *key, size_t keylen, pageid_t ptr);
    //reads the given record and returns the page id stored in it
    static pageid_t lookupLeafPageFromRid(int xid, recordid rid);

    recordid appendInternalNode(int xid, Page *p,
                                       int64_t depth,
                                       const byte *key, size_t key_len,
                                       pageid_t val_page);

    recordid buildPathToLeaf(int xid, recordid root, Page *root_p,
                                    int64_t depth, const byte *key, size_t key_len,
                                    pageid_t val_page);

    /**
       Initialize a page for use as an internal node of the tree.
     */
    inline static void initializeNodePage(int xid, Page *p);

    //return the left-most leaf, these are not data pages, although referred to as leaf
    static pageid_t findFirstLeaf(int xid, Page *root, int64_t depth);
    //return the right-most leaf
    static pageid_t findLastLeaf(int xid, Page *root, int64_t depth) ;

    //returns a record that stores the pageid where the given key should be in, i.e. if it exists
    static recordid lookup(int xid, Page *node, int64_t depth, const byte *key,
                           size_t keySize);

    const static int64_t DEPTH;
    const static int64_t COMPARATOR;
    const static int64_t FIRST_SLOT;
    const static ssize_t root_rec_size;
    const static int64_t PREV_LEAF;
    const static int64_t NEXT_LEAF;
    pageid_t lastLeaf;

    void print_tree(int xid, pageid_t pid, int64_t depth);

    recordid root_rec;
    regionAllocator* internal_node_alloc;
    regionAllocator* datapage_alloc;

    struct indexnode_rec {
      pageid_t ptr;
    };

  public:
    class iterator {
    public:
      iterator(int xid, regionAllocator *ro_alloc, recordid root);
      iterator(int xid, regionAllocator *ro_alloc, recordid root, const byte* key, len_t keylen);
      int next();
      void close();

      inline size_t key (byte **key) {
        *key = (byte*)(t+1);
        return current.size - sizeof(indexnode_rec);
      }

      inline size_t value(byte **value) {
        *value = (byte*)&(t->ptr);
        return sizeof(t->ptr);
      }

      inline void tupleDone() { }
      inline void releaseLock() { }

    private:
      regionAllocator * ro_alloc_;
      Page * p;
      int xid_;
      bool done;
      recordid current;
      indexnode_rec *t;
      int justOnePage;

    };
  };

  stasis_bloom_filter_t * bloom_filter;

  class iterator
  {

  public:
      explicit iterator(diskTreeComponent::internalNodes *tree, mergeManager * mgr = NULL, double target_size = 0, bool * flushing = NULL);

      explicit iterator(diskTreeComponent::internalNodes *tree,dataTuple *key);

      ~iterator();

      dataTuple * next_callerFrees();

  private:
    void init_iterators(dataTuple * key1, dataTuple * key2);
    inline void init_helper(dataTuple * key1);

    explicit iterator() { abort(); }
    void operator=(iterator & t) { abort(); }
    int operator-(iterator & t) { abort(); }

    regionAllocator * ro_alloc_;  // has a filehandle that we use to optimize sequential scans.
    recordid tree_; //root of the tree
    mergeManager * mgr_;
    double   target_progress_delta_;
    bool * flushing_;

    diskTreeComponent::internalNodes::iterator* lsmIterator_;

    pageid_t curr_pageid; //current page id
    dataPage *curr_page;   //current page
    typedef dataPage::iterator DPITR_T;
    DPITR_T *dp_itr;

  };
};
#endif /* DISKTREECOMPONENT_H_ */
