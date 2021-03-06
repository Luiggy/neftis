/*
 *    Virtual memory subsystem.
 *    Copyright (C) 2014  Gonzalo J. Carracedo
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
 
#include <types.h>

#include <mm/regions.h>
#include <mm/coloring.h>
#include <mm/vm.h>
#include <mm/anon.h>
#include <mm/spalloc.h>

#include <misc/list.h>
#include <task/loader.h>

#include <arch.h>
#include <kctx.h>


struct vm_region *
vm_region_new (busword_t start, busword_t end, struct vm_region_ops *ops, void *data)
{
  struct vm_region *new;
  
  CONSTRUCT_STRUCT (vm_region, new);

  new->vr_ops = ops;
  new->vr_ops_data = data;
  new->vr_virt_start = start;
  new->vr_virt_end   = end;
  
  return new;
}

void
vm_region_destroy (struct vm_region *region, struct task *task)
{
  if (region->vr_ops->destroy != NULL)
    (region->vr_ops->destroy) (task, region);

  if (region->vr_type == VREGION_TYPE_PAGEMAP)
    radix_tree_destroy (region->vr_page_tree);
  
  spfree (region);
}

int
vm_region_map_page (struct vm_region *region, busword_t virt, busword_t phys, DWORD flags)
{
  void **slot;

  if (region->vr_type == VREGION_TYPE_RANGEMAP)
    return KERNEL_ERROR_VALUE;
  
  /* TODO: define radix_tree_set_with_flags */
  if (radix_tree_set (&region->vr_page_tree, virt, (void *) phys) == KERNEL_ERROR_VALUE)
    return KERNEL_ERROR_VALUE;

  if (radix_tree_set_tag (region->vr_page_tree, virt, flags) == -1)
    FAIL ("Cannot set tag on existing mapped page?\n");

  return KERNEL_SUCCESS_VALUE;
}

busword_t
vm_region_translate_page (struct vm_region *region, busword_t virt, DWORD *flags)
{
  void **addr;
  radixtag_t *tag;

  if (region->vr_type == VREGION_TYPE_RANGEMAP)
  {
    if (virt < region->vr_virt_start || virt > region->vr_virt_end)
      return (busword_t) KERNEL_ERROR_VALUE;

    return virt - region->vr_virt_start + region->vr_phys_start;
  }
  else
  {
    /* Really, this sucks */
    if ((addr = radix_tree_lookup_slot (region->vr_page_tree, virt)) == NULL)
      return (busword_t) KERNEL_ERROR_VALUE;

    if (flags != NULL)
    {
      if ((tag = radix_tree_lookup_tag (region->vr_page_tree, virt)) == NULL)
	return (busword_t) KERNEL_ERROR_VALUE;

      *flags = *tag;
    }
  }
  
  return (busword_t) *addr;
}

struct vm_space *
vm_space_new (void)
{
  CONSTRUCTOR_BODY_OF_STRUCT (vm_space);
}

void
vm_space_destroy (struct vm_space *space)
{
  struct vm_region *region, *next;
  
  region = space->vs_regions;
  
  while (region)
  {
    next = LIST_NEXT (region);
    
    vm_region_destroy (region, NULL);
    
    region = next;
  }
  
  __vm_free_page_table (space->vs_pagetable);
}

/* Next level */

INLINE int
vm_test_range (struct vm_space *space, busword_t start, busword_t pages)
{
  struct vm_region *region_prev, *region_next;
  
  region_prev = 
    sorted_list_get_previous ((void **) &space->vs_regions, start);
    
  region_next = 
    sorted_list_get_next ((void **) &space->vs_regions, start);
    
  if (region_prev != NULL)
    if (region_prev->vr_virt_end >= start)
      return 0;
    
  if (region_next != NULL)
    if (region_next->vr_virt_start < (start + pages * PAGE_SIZE - 1))
      return 0;
    
  
  return 1;
}

static int
__invalidate_pages (radixkey_t virt, void **phys, radixtag_t *perms, void *data)
{
  __vm_flush_pages ((busword_t) virt, 1);

  return KERNEL_SUCCESS_VALUE;
}

void
vm_region_invalidate (struct vm_region *region)
{
  radix_tree_walk (region->vr_page_tree, __invalidate_pages, NULL);
}

int
vm_space_add_region (struct vm_space *space, struct vm_region *region)
{
  if (!vm_test_range (space, 
                      region->vr_virt_start,
                      (region->vr_virt_end + 1 - region->vr_virt_start) /
                        PAGE_SIZE))
  {
    error ("Map region %p-%p: overlapping regions!\n", region->vr_virt_start, region->vr_virt_end);
    return KERNEL_ERROR_VALUE;
  }
  
  sorted_list_insert ((void *) &space->vs_regions,
                      region,
                      region->vr_virt_start);


  vm_update_region (space, region);
  
  return KERNEL_SUCCESS_VALUE;
}

int
vm_space_overlap_region (struct vm_space *space, struct vm_region *region)
{
  sorted_list_insert ((void *) &space->vs_regions,
                      region,
                      region->vr_virt_start);

  vm_update_region (space, region);
  
  return 0;
}

busword_t
virt2phys (const struct vm_space *space, busword_t virt)
{
  struct vm_region *curr;
  busword_t off  = virt &  PAGE_MASK;
  busword_t page = virt & ~PAGE_MASK;
  busword_t phys;
  
  curr = space->vs_regions;

  while (curr)
  {
    if (virt >= curr->vr_virt_start && virt <= curr->vr_virt_end)
    {
      if ((phys = vm_region_translate_page (curr, page, NULL)) != (busword_t) KERNEL_ERROR_VALUE)
        return phys | off;
      else
        return 0;
    }
    
    curr = LIST_NEXT (curr);
  }

  return 0;
}


int
copy2virt (const struct vm_space *space, busword_t virt, const void *orig, busword_t size)
{
  busword_t offset = virt &  PAGE_MASK; 
  busword_t page   = virt & ~PAGE_MASK;
  busword_t phys, len;
  
  /* TODO: implement this faster */

  while (size)
  {
    if (!(phys = virt2phys (space, page)))
      break;
    
    if ((len = PAGE_SIZE - offset) > size)
      len = size;

    memcpy (phys + offset, orig, len);

    orig += len;
    page += PAGE_SIZE;
    size -= len;
    
    offset = 0;
  }

  return size;
}

static int
__map_pages (radixkey_t virt, void **phys, radixtag_t *perms, void *data)
{
  struct vm_space *space = data;
  
  return __vm_map_to (space->vs_pagetable, (busword_t) virt, (busword_t) *phys, 1, *perms);
}

int
vm_update_region (struct vm_space *space, struct vm_region *region)
{
  if (space->vs_pagetable == NULL)
    space->vs_pagetable = __vm_alloc_page_table ();

  if (region->vr_type == VREGION_TYPE_RANGEMAP)
    return __vm_map_to (space->vs_pagetable, region->vr_virt_start, region->vr_phys_start, __UNITS (region->vr_virt_end - region->vr_virt_start + 1, PAGE_SIZE), region->vr_access);
  else
    return radix_tree_walk (region->vr_page_tree, __map_pages, space);
}

/* Transforms segment information into hardware translation data (i.e.
   update page tables, etc) */
int
vm_update_tables (struct vm_space *space)
{
  struct vm_region *this;
  int ret;
  
  this = space->vs_regions;
  
  while (this)
  {
    if ((ret = vm_update_region (space, this)) != KERNEL_SUCCESS_VALUE)
      return ret;
    
    this = LIST_NEXT (this);
  }
  
  return KERNEL_SUCCESS_VALUE;
}

struct vm_space *
vm_kernel_space (void)
{
  extern struct mm_region *mm_regions;
  
  struct vm_space  *new_space;
  struct vm_region *new_region;
  struct mm_region *this;
  
  PTR_RETURN_ON_PTR_FAILURE (new_space = vm_space_new ());

  this = mm_regions;
  
  while (this != NULL)
  { 
    if (PTR_UNLIKELY_TO_FAIL (new_region = vm_region_physmap ((busword_t) this->mr_start, __UNITS ((busword_t) this->mr_end - (busword_t) this->mr_start + 1, PAGE_SIZE), VREGION_ACCESS_READ | VREGION_ACCESS_WRITE)))
    {
      error ("Failed to register VM kernel space\n");
      
      vm_space_destroy (new_space);
      return KERNEL_INVALID_POINTER;
    }
                                
    if (UNLIKELY_TO_FAIL (vm_space_overlap_region (new_space, new_region)))
    {
      error ("Failed to add region to VM kernel space\n");
      vm_region_destroy (new_region, NULL);
      vm_space_destroy (new_space);
      
      return KERNEL_INVALID_POINTER;
    }

    this = this->mr_next;
  }
  
  if (UNLIKELY_TO_FAIL (vm_kernel_space_map_image (new_space)))
  {
    error ("couldn't map kernel image\n");
    
    vm_space_destroy (new_space);
    
    return KERNEL_INVALID_POINTER;
  }

  if (UNLIKELY_TO_FAIL (vm_kernel_space_map_io (new_space)))
  {
    error ("Cannot map IO!\n");

    vm_space_destroy (new_space);
    
    return KERNEL_INVALID_POINTER;
  }

  
  return new_space;
}

struct vm_space *
vm_bare_sysproc_space (void)
{
  extern struct mm_region *mm_regions;
  busword_t stack_bottom;
  struct vm_space  *new_space;
  struct vm_region *stack;
  
  PTR_RETURN_ON_PTR_FAILURE (new_space = vm_space_new ());

  /* Map kernel space */
  
  if (UNLIKELY_TO_FAIL (vm_kernel_space_map_image (new_space)))
  {
    error ("couldn't map kernel image\n");
    
    vm_space_destroy (new_space);
    
    return KERNEL_INVALID_POINTER;
  }

  return new_space;
}

static int
__load_segment_cb (struct vm_space *space, int type, int flags, busword_t virt, busword_t size, const void *data, busword_t datasize)
{
  struct vm_region *region;
  busword_t start_page;
  busword_t actual_size;
  busword_t actual_page_count;
  busword_t pending;

  start_page = PAGE_START (virt);
  actual_size = size + (virt - start_page);
  actual_page_count = (actual_size >> __PAGE_BITS) + !!(actual_size & PAGE_MASK);

  if (datasize > size)
  {
    error ("Segment data size overflows segment size\n");
    return -1;
  }

  if ((region = vm_region_anonmap (start_page, actual_page_count, flags)) == KERNEL_INVALID_POINTER)
    return -1;

  if (vm_space_add_region (space, region) != KERNEL_SUCCESS_VALUE)
  {
    error ("Segment collision (cannot load %p-%p)\n", start_page, start_page + (actual_page_count << __PAGE_BITS) - 1);

    vm_region_destroy (region, NULL);
    
    return -1;
  }
  
  if ((pending = copy2virt (space, virt, data, datasize)) != 0)
    FAIL ("unexpected segment overrun when copying data to user\n");

  return 0;
}

struct vm_space *
vm_space_load_from_exec (const void *exec_start, busword_t exec_size, busword_t *entry)
{
  struct vm_space  *space;
  loader_handle    *handle;

  PTR_RETURN_ON_PTR_FAILURE (space = vm_bare_sysproc_space ());

  if ((handle = loader_open_exec (space, exec_start, exec_size)) == KERNEL_INVALID_POINTER)
  {
    vm_space_destroy (space);
    return KERNEL_INVALID_POINTER;
  }

  if (loader_walk_exec (handle, __load_segment_cb) == KERNEL_ERROR_VALUE)
  {
    vm_space_destroy (space);
    return KERNEL_INVALID_POINTER;
  }

  if (entry != NULL)
    *entry = loader_get_exec_entry (handle);
  
  loader_close_exec (handle);

  return space;
}

void
vm_space_debug (struct vm_space *space)
{
  struct vm_region *this;
  
  this = space->vs_regions;
  
  while (this)
  {
    printk ("%y-%y: %s %H\n", this->vr_virt_start,
      this->vr_virt_end,
      this->vr_ops->name,
      this->vr_virt_end - this->vr_virt_start + 1);
      
    this = (struct vm_region *) LIST_NEXT (this);
  }
}

void
vm_init (void)
{ 
  /* TODO: fix for SMP */
  
  MANDATORY (SUCCESS_PTR (
    current_kctx->kc_vm_space = vm_kernel_space ()
    )
  );

  hw_vm_init ();
}

DEBUG_FUNC (vm_region_new);
DEBUG_FUNC (vm_region_destroy);
DEBUG_FUNC (vm_region_map_page);
DEBUG_FUNC (vm_region_translate_page);
DEBUG_FUNC (vm_space_new);
DEBUG_FUNC (vm_space_destroy);
DEBUG_FUNC (vm_test_range);
DEBUG_FUNC (__invalidate_pages);
DEBUG_FUNC (vm_region_invalidate);
DEBUG_FUNC (vm_space_add_region);
DEBUG_FUNC (vm_space_overlap_region);
DEBUG_FUNC (vm_region_stack);
DEBUG_FUNC (vm_region_kernel_stack);
DEBUG_FUNC (vm_region_iomap);
DEBUG_FUNC (virt2phys);
DEBUG_FUNC (copy2virt);
DEBUG_FUNC (__map_pages);
DEBUG_FUNC (vm_update_region);
DEBUG_FUNC (vm_update_tables);
DEBUG_FUNC (vm_kernel_space);
DEBUG_FUNC (vm_bare_sysproc_space);
DEBUG_FUNC (__load_segment_cb);
DEBUG_FUNC (vm_space_load_from_exec);
DEBUG_FUNC (vm_space_debug);
DEBUG_FUNC (vm_init);
