
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "sfmm.h"

/*added by myself*/
#include <errno.h>

#define PACK(size,info) ((size)|(info)) /*pack a size, a in_aklst, a prv_alloc and a alloc bit*/
#define GET(p) (*(size_t *)(p))      /*Read a row at address p*/
#define PUT(p, val) (*((size_t *)(p)) = (val)) /*Write a row at address p*/
/*get and set*/
#define GET_SIZE(p) (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)
#define GET_PREV_ALLOC(p) (GET(p) & 0x2)
#define GET_IN_QKLST(p) (GET(p) & 0x4)

#define GET_FTRP(p) ((char *)(p) + GET_SIZE(p) - 8)
#define GET_NEXT(p) ((char *)(p) + GET_SIZE(p))
#define GET_PREV(p) ((char *)(p) - GET_SIZE(((char *)(p) - 8)))

#define SET_SIZE(p, size) (PUT(p, PACK(size, GET(p) & 0x7)))
#define SET_ALLOC(p) (PUT(p, GET(p) | 0x1))
#define SET_FREE(p) (PUT(p, GET(p) & ~0x1))
#define SET_PREV_ALLOC(p) (PUT(p, GET(p) | 0x2))
#define SET_PREV_FREE(p) (PUT(p, GET(p) & ~0x2))
#define SET_IN_QKLST(p) (PUT(p, GET(p) | 0x4))
#define SET_NOT_IN_QKLST(p) (PUT(p, GET(p) & ~0x4))


#define MIN_BLOCK_SIZE 32
#define ALIGNMENT_SIZE 8

int get_free_list_index(size_t size);
int get_quick_list_index(size_t size);

void add_block_to_free(sf_block * block);
void add_block_to_quick(sf_block *block);

void remove_free_asalloc(sf_block *rm_block);
void remove_free(sf_block *rm_block);

size_t calculate_block_size(size_t size);
void *calculate_mem(void *pp, size_t align);
void split_block(sf_block *bp, size_t size);
sf_block *search_free(size_t bsize);
sf_block *coalesce(sf_block *block);

int invalid_pointer (void *pp);
int valid_align(size_t align);

char *heap_start;
char *prologue_start;
char *epilogue_start;

void *sf_malloc(size_t size) {
    if (size <= 0)
        return NULL;

    /*initial heap and all list*/
    if (sf_mem_end()==sf_mem_start()){

        /*"dummy" block initial for every free list*/
        for (int i = 0; i < NUM_FREE_LISTS; i++){
            /*In an empty list, the next and free pointers of the list header point back to itself.*/
            sf_free_list_heads[i].body.links.prev = &sf_free_list_heads[i];
            sf_free_list_heads[i].body.links.next = &sf_free_list_heads[i];
        }
        heap_start = sf_mem_grow();
        if(!heap_start){
            sf_errno = ENOMEM;
            return NULL;
        }

        /*set prologue*/
        prologue_start = heap_start;
        char *prologue_end = prologue_start + 32;
        PUT(prologue_start, PACK(32,0x1));/*header*/
        PUT(prologue_start+24, PACK(32,0x1));/*footer*/
        /*set epilogue*/
        epilogue_start= ((char *)sf_mem_end()) - 8;
        PUT(epilogue_start, PACK(0,1));/*header only*/

        /*initial free block*/
        int s = epilogue_start- prologue_end;
        PUT(prologue_end, PACK(s,0x2));
        PUT(epilogue_start-8, PACK(s,0x2));
        int index_infree = get_free_list_index(GET_SIZE(prologue_end));
        sf_block *first_free = (sf_block *)prologue_end;
        first_free->body.links.prev = &sf_free_list_heads[index_infree];
        first_free->body.links.next = &sf_free_list_heads[index_infree];
        sf_free_list_heads[index_infree].body.links.prev = first_free;
        sf_free_list_heads[index_infree].body.links.next = first_free;
    }

    /*get the blocksize needed*/
    size_t blocksize = calculate_block_size(size);

    /*search quik list first*/
    sf_block *ptr;
    sf_block *new_first;
    int index_inquik = get_quick_list_index(blocksize);
    if(index_inquik <= (NUM_QUICK_LISTS-1)&&index_inquik >= 0){/*check if the index valid (in quick list)*/
        if(sf_quick_lists[index_inquik].length != 0){/*check if there is a quick list but it is empty*/
            ptr = sf_quick_lists[index_inquik].first;/*reserve address of first block*/
            new_first = ptr->body.links.next;/*get the address of next block and set it to first*/
            sf_quick_lists[index_inquik].first = new_first;
            sf_quick_lists[index_inquik].length -=1;
            SET_NOT_IN_QKLST(ptr);
            ptr->body.links.next = NULL;/*clean the next pointer for the allocated block*/
            return ptr->body.payload;
        }
    }

    /*search free list then*/
    sf_block *found_block = search_free(blocksize);
    if (found_block!=NULL){
        split_block(found_block,blocksize);
        return found_block->body.payload;
    }else{
        //extend heap if no found
        sf_block *new_heap;
        sf_block *new_epilogue;
        sf_block *new_block;
        size_t bsize;
        while((new_heap = sf_mem_grow())!=NULL){
            new_epilogue = (sf_block*)((char *)sf_mem_end()-8);
            /*move epilogue*/
            PUT(new_epilogue, PACK(0,1));
            /*create new block*/
            new_block = (sf_block *)epilogue_start;
            epilogue_start = (char *)new_epilogue;
            bsize = (size_t)new_epilogue - (size_t)new_block;
            SET_SIZE(new_block,bsize);
            SET_FREE(new_block);
            add_block_to_free(new_block);

            if((found_block=search_free(blocksize))!=NULL){
                split_block(found_block,blocksize);
                return found_block->body.payload;
            }
        }
    }
    exit(0);
}

void sf_free(void *pp) {
    if(invalid_pointer(pp)){
        sf_header *header = (sf_header *)((char *)pp - 8);
        size_t bsize = GET_SIZE(header);
        memset(pp,0,bsize-8); // clean the payload erea
        int idx_quik = get_quick_list_index(bsize);// check the size of the block to see if it fits a quick list
        if(idx_quik >=0 && idx_quik <= (NUM_QUICK_LISTS-1)){
            add_block_to_quick((sf_block *)header); // if it fits then add it to a quick list
        }else{
            SET_FREE(header);
            add_block_to_free((sf_block *)header); // if not fit, add it to a free list;
        }
    }else{
        abort();
    }
}

void *sf_realloc(void *pp, size_t rsize) {
    if(!invalid_pointer(pp)){
        sf_errno = EINVAL;
        return NULL;
    }
    if(rsize == 0){
        sf_free(pp);
        return NULL;
    }
    sf_header *header = (sf_header *)((char *)pp - 8);
    size_t block_size = calculate_block_size(rsize);
    size_t current_size = GET_SIZE(header);
    //3 cases, smaller, biger or same
    if(block_size<current_size){
        split_block((sf_block *)header,block_size);
        return pp;
    }else if(block_size>current_size){
        sf_block *new_block;
        if((new_block=sf_malloc(rsize))==NULL){
            return NULL;
        }
        memcpy(new_block,pp,(current_size-8));
        sf_free(pp);
        return new_block;
    }else{
        return pp;
    }

    abort();
}

void *sf_memalign(size_t size, size_t align) {
    if(!valid_align(align)){
        sf_errno = EINVAL;
        return NULL;
    }
    if((int)size==0){
        return NULL;
    }
    size_t bsize = align+MIN_BLOCK_SIZE+size+8;
    /* attempts to allocate a block
     * whose size is at least the requested size, plus the alignment size, plus the minimum
     * block size, plus the size required for a block header and footer.
     * sf_block *mem_block = sf_malloc(bsize);
     */
    void *mem_block = sf_malloc(bsize);
    sf_block *prev_header = (sf_block *)((char *)mem_block - 8);
    size_t original_s = GET_SIZE(prev_header);

    if ((size_t)mem_block % align != 0){
        void *temp = calculate_mem(mem_block,align);
        sf_block *temp_header = (sf_block *)((char *)temp - 8);
        size_t pre_size = (size_t)temp-(size_t)mem_block;
        //set new size of the pre header
        SET_SIZE(prev_header,pre_size);
        SET_FREE(prev_header);
        add_block_to_free(prev_header);

        SET_SIZE(temp_header,(original_s-pre_size));
        SET_PREV_FREE(temp_header);
        SET_ALLOC(temp_header);
        mem_block = temp;

    }
    size_t size_need = calculate_block_size(size);
    split_block((sf_block *)((char *)mem_block - 8),size_need);
    return mem_block;
}

/*helper function below*/

/*get the index by checking the given size, index begin from 0*/
int get_free_list_index(size_t size) {
    int index = 0;
    size_t block_size = 32;
    while (index < NUM_FREE_LISTS-1 && block_size < size) {
        block_size <<= 1; // equivalent to multiplying by 2
        index++;
    }
    return index;
}
/*get the index by checking the given size, important: index begin from 0 here! -1 indicate false*/
int get_quick_list_index(size_t size) {
    if (size < MIN_BLOCK_SIZE){
        return -1;
    }else if (size == MIN_BLOCK_SIZE) {
        return 0;
    } else {
        return (size - MIN_BLOCK_SIZE) / ALIGNMENT_SIZE;
    }
}

size_t calculate_block_size(size_t size) {
    const size_t block_size = size + sizeof(sf_header); // Add header size
    if(block_size < 32){
        return 32; // Min size is 32
    }
    const size_t remainder = block_size % 8;
    if (remainder == 0) {
        return block_size;
    } else {
        return block_size + 8 - remainder; // Align block size to 8 bytes (one row)
    }
}

void* calculate_mem(void* addr, size_t align) {
    void* aligned = addr;
    size_t offset = align - ((size_t)addr % align);
    aligned += offset;
    // Ensure output address is at least 32 bytes away from input address
    while ((size_t)aligned - (size_t)addr < 32) {
        aligned += align;
    }
    return aligned;
}

void split_block(sf_block *bp, size_t size){
    sf_header *header = (sf_header*)bp;//header of the original block
    size_t block_size = GET_SIZE(header);
    sf_footer *next_footer = (sf_footer *)((char *)bp + block_size - 8);  //set pointer to the footer of the new block
    if((block_size-size) >= 32){ //split if diff bigger then 32
        sf_header* next_header = (sf_header *)((char *)bp + size);//header of the new block
        SET_ALLOC(header);//set allocator bit and prev alloc bit for first block header
        size_t new_size = block_size-size;
        SET_SIZE(next_header,new_size);// set header for new block with new block size
        SET_FREE(next_header);
        SET_PREV_ALLOC(next_header);   //set header for new block with alloc bit(0) and prev_alloc bit(1)
        SET_SIZE(header,size);

        *(size_t *)next_footer = *((size_t *)((char *)next_header));  //set footer with same value
        //insert new free block in the free list, add it to an appropriate size list, coalesce if possible
        add_block_to_free((sf_block *)next_header);
    }
    return;

}

/*search the free list by the given bsize, remove the block from free list*/
sf_block *search_free(size_t bsize){
    int index_infree = get_free_list_index(bsize);
    sf_block *ptr2;
    sf_block *dummy;
    for (int i = index_infree; i < NUM_FREE_LISTS; i++){
        dummy = &sf_free_list_heads[i];
        ptr2 = (sf_free_list_heads[i]).body.links.next;
        while(ptr2!=dummy){
            if(bsize <= GET_SIZE(ptr2)){
                remove_free_asalloc(ptr2);
                return ptr2;
            }
            ptr2 = ptr2->body.links.next;
        }
    }
    return NULL;
}

/*remove block from list, clean the prev and next area, set alloc to 1, remove footer */
void remove_free_asalloc(sf_block *rm_block){
    size_t block_size = GET_SIZE(rm_block);
    sf_footer * footer = (sf_footer *) ((char *)rm_block + block_size - 8);
    PUT(footer,0); // clean the footer erea
    SET_ALLOC(rm_block);
    remove_free(rm_block);
    SET_PREV_ALLOC((char *)rm_block + (block_size));//change prev_alloc bit for next block in heap
    return;
}
/*simply remove the block from free list, prev and next set to null*/
void remove_free(sf_block *rm_block){
    if(rm_block->body.links.prev!=NULL && rm_block->body.links.next!=NULL){
        (rm_block->body.links.prev)->body.links.next = rm_block->body.links.next;
        (rm_block->body.links.next)->body.links.prev = rm_block->body.links.prev;
        rm_block->body.links.prev = NULL;
        rm_block->body.links.next = NULL;
    }
    return;
}

/*insert new free block in the free list, argument is a free list without pre and next*/
void add_block_to_free(sf_block * block){
    block = coalesce(block);
    size_t bsize = GET_SIZE(block);
    int free_index = get_free_list_index(bsize);
    /*insert it in the head of the list*/
    block->body.links.next = sf_free_list_heads[free_index].body.links.next;
    block->body.links.prev = &sf_free_list_heads[free_index];
    (block->body.links.next)->body.links.prev = block;
    sf_free_list_heads[free_index].body.links.next = block;
    PUT(GET_FTRP(block),GET(block));/*make sure foot = header*/
    SET_PREV_FREE(GET_NEXT(block));//make sure the next block set prev_alloc bit to 0
}
/*insert new free block in the free list, argument is a free list with alloc set to 1*/
void add_block_to_quick(sf_block *block){
    size_t bsize = GET_SIZE(block);
    int idx_quik = get_quick_list_index(bsize);
    if(sf_quick_lists[idx_quik].length == QUICK_LIST_MAX){//if the length of the quick list reached the max, then flush it
        sf_block *ptr = sf_quick_lists[idx_quik].first;
        sf_block *next = ptr->body.links.next;
        //clean first and length attribute in lists
        sf_quick_lists[idx_quik].first = NULL;
        sf_quick_lists[idx_quik].length = 0;
        for ( int i = 0; i < QUICK_LIST_MAX; i++){
            SET_FREE(ptr);
            SET_NOT_IN_QKLST(ptr);
            ptr->body.links.next = NULL;//clean next area
            add_block_to_free(ptr);
            ptr = next;
            if(ptr!=NULL){
                next = ptr->body.links.next;
            }
        }

    }
    SET_IN_QKLST(block);
    block->body.links.next = sf_quick_lists[idx_quik].first;
    sf_quick_lists[idx_quik].first = block;
    sf_quick_lists[idx_quik].length +=1;
}
/*insert new free block in the quick list, argument is a free list*/

/*note: argument - a block that not in list*/
sf_block *coalesce(sf_block *block){
    size_t bsize = GET_SIZE(block);
    size_t prev_alloc = GET_PREV_ALLOC(block);
    size_t next_alloc = GET_ALLOC((char *)block + bsize);
    sf_block *next_block = (sf_block *)GET_NEXT(block);
    sf_block *new_block;

    /*4 situation below 1. alloc free alloc 2. free free free 3. free free alloc 4. alloc free free*/
    if(prev_alloc && next_alloc){
        SET_PREV_FREE(next_block);//make sure the next block set prev_alloc bit to 0
        new_block = block;
    }else if(!prev_alloc && !next_alloc){
        sf_block *prev_block = (sf_block *)GET_PREV(block);
        remove_free(next_block);
        remove_free(prev_block);
        size_t new_size = GET_SIZE(next_block) + bsize + GET_SIZE(prev_block);
        SET_SIZE(prev_block, new_size);/*set the size of previous block header to be new_size*/
        PUT(GET_FTRP(next_block),GET(prev_block));/*set the next block footer to be the same as header*/
        /*clean prev footer, current header, current footer, next header*/
        PUT(GET_FTRP(prev_block),0);
        PUT(block,0);
        PUT(GET_FTRP(block),0);
        PUT(next_block,0);
        new_block = prev_block;
    }else if(!prev_alloc && next_alloc){
        sf_block *prev_block = (sf_block *)GET_PREV(block);
        remove_free(prev_block);
        size_t new_size = GET_SIZE(prev_block) + bsize;
        SET_SIZE(prev_block, new_size);/*set the size of previous block header to be new_size*/
        PUT(GET_FTRP(block),GET(prev_block));/*set the current block footer to be the same as header*/
        /*clean prev footer, current header*/
        PUT(GET_FTRP(prev_block),0);
        PUT(block,0);
        new_block = prev_block;
    }else if(prev_alloc && !next_alloc){
        remove_free(next_block);
        size_t new_size  = GET_SIZE(next_block) + bsize;
        SET_SIZE(block,new_size);/*set the size of current block header to be new_size*/
        PUT(GET_FTRP(next_block),GET(block));/*set the next block footer to be the same as header*/
        /*clean current footer, next header*/
        PUT(GET_FTRP(block),0);
        PUT(next_block,0);
        new_block = block;
    }else{
        abort();
    }
    return new_block;
}

/*return 0 if invalid, 1 if valid*/
int invalid_pointer (void *pp){
    sf_header *header = (sf_header *)((char *)pp - 8);
    if(pp == NULL){
        return 0;
    }//null pointer, invalid

    if(((size_t)pp - (size_t)heap_start)%8 != 0){
        return 0;
    }//not 8 bytes aligned, invalid

    if(GET_SIZE(header)<32){
        return 0;
    }//The block size is less than the minimum block size of 32, invalid

    if(GET_SIZE(header)%8 != 0){
        return 0;
    }//The block size is not a multiple of 8

    if(GET_ALLOC(header)==0){
        return 0;
    }//The allocated bit in the header is 0, invalid

    if(GET_IN_QKLST(header)==IN_QUICK_LIST){
        return 0;
    }//The in quick list bit in the header is 1, invalid

    if((size_t)header < (size_t)heap_start){
        return 0;
    }
    if((size_t)header > (size_t)((char*)epilogue_start + 8)){
        return 0;
    }//The header of the block is before the start of the first block of the heap, or the footer of the block is after the end of the last block in the heap.


    if(GET_PREV_ALLOC(header) == 0){
        sf_header *prev_header = (sf_header *)GET_PREV(header);
        if(GET_ALLOC(prev_header) == THIS_BLOCK_ALLOCATED){
            return 0;
        }
    }//The prev_alloc field in the header is 0, indicating that the previous block is free, but the alloc field of the previous block header is not 0.
    return 1;
}
int valid_align(size_t align){
    if (align < 8){
        return 0;
    }

    while(align!=1){
        if(align % 2 != 0){
            return 0;
        }
        align = align/2;
    }
    return 1;
}
