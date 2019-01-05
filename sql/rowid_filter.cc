#include "mariadb.h"
#include "table.h"
#include "sql_class.h"
#include "opt_range.h"
#include "rowid_filter.h"
#include "sql_select.h"


/**
  Sets information about filter with key_numb index.
  It sets a cardinality of filter, calculates its selectivity
  and gets slope and interscept values.
*/

void Range_filter_cost_info::init(TABLE *tab, uint key_numb)
{
  table= tab;
  key_no= key_numb;
  est_elements= table->quick_rows[key_no];
  b= build_cost(ORDERED_ARRAY_CONTAINER);
  selectivity= est_elements/((double) table->stat_records());
  a= (1 + COST_COND_EVAL)*(1 - selectivity) - lookup_cost();
  intersect_x_axis_abcissa= b/a;
}

double
Range_filter_cost_info::build_cost(Rowid_filter_container_type container_type)
{
  double cost= 0;

  cost+= table->quick_index_only_costs[key_no];

  switch (container_type) {

  case ORDERED_ARRAY_CONTAINER:
    cost+= ARRAY_WRITE_COST * est_elements; /* cost filling the container */
    cost+= ARRAY_SORT_C * est_elements * log(est_elements); /* sorting cost */
    break;
  default:
    DBUG_ASSERT(0);
  }

  return cost;
}

/**
  @brief
    Sort available filters by their building cost in the increasing order

  @details
    The method starts sorting available filters from the first filter that
    is not defined as the best filter. If there are two filters that are
    defined as the best filters there is no need to sort other filters.
    Best filters are already sorted by their building cost and have the
    smallest bulding cost in comparison with other filters by definition.

    As the sorting method bubble sort is used.
*/

void TABLE::sort_range_filter_cost_info_array()
{
  if (best_filter_count  <= 2)
    return;

  for (uint i= best_filter_count; i < range_filter_cost_info_elements-1; i++)
  {
    for (uint j= i+1; j < range_filter_cost_info_elements; j++)
    {
      if (range_filter_cost_info[i].intersect_x_axis_abcissa >
          range_filter_cost_info[j].intersect_x_axis_abcissa)
        swap_variables(Range_filter_cost_info,
                       range_filter_cost_info[i],
                       range_filter_cost_info[j]);
    }
  }
}


/**
  @brief
    The method searches for the filters that can reduce the join cost the most

  @details
    The method looks through the available filters trying to choose the best
    filter and eliminate as many filters as possible.

    Filters are considered as a linear functions. The best filter is the linear
    function that intersects all other linear functions not in the I quadrant
    and has the biggest a (slope) value. This filter will reduce the partial
    join cost the most. If it is possible the second best filter is also
    chosen. The second best filter can be used if the ref access is made on
    the index of the first best filter.

    So there is no need to store all other filters except filters that
    intersect in the I quadrant. It is impossible to say on this step which
    filter is better and will give the biggest gain.

    The number of filters that can be used is stored in the
    range_filter_cost_info_elements variable.
*/

void TABLE::prune_range_filters()
{
  key_map pruned_filter_map;
  pruned_filter_map.clear_all();
  Range_filter_cost_info *max_slope_filters[2] = {0, 0};

  for (uint i= 0; i < range_filter_cost_info_elements; i++)
  {
    Range_filter_cost_info *filter= &range_filter_cost_info[i];
    if (filter->a < 0)
    {
      range_filter_cost_info_elements--;
      swap_variables(Range_filter_cost_info, range_filter_cost_info[i],
                     range_filter_cost_info[range_filter_cost_info_elements]);
      i--;
      continue;
    }
    for (uint j= i+1; j < range_filter_cost_info_elements; j++)
    {
      Range_filter_cost_info *cand_filter= &range_filter_cost_info[j];

      double intersect_x= filter->get_intersect_x(cand_filter);
      double intersect_y= filter->get_intersect_y(intersect_x);

      if (intersect_x > 0 && intersect_y > 0)
      {
        pruned_filter_map.set_bit(cand_filter->key_no);
        pruned_filter_map.set_bit(filter->key_no);
      }
    }
    if (!pruned_filter_map.is_set(filter->key_no))
    {
      if (!max_slope_filters[0])
        max_slope_filters[0]= filter;
      else
      {
        if (!max_slope_filters[1] ||
            max_slope_filters[1]->a < filter->a)
          max_slope_filters[1]= filter;
        if (max_slope_filters[0]->a < max_slope_filters[1]->a)
          swap_variables(Range_filter_cost_info*, max_slope_filters[0],
                                                  max_slope_filters[1]);
      }
    }
  }

  for (uint i= 0; i<2; i++)
  {
    if (max_slope_filters[i])
    {
      swap_variables(Range_filter_cost_info,
                     range_filter_cost_info[i],
                     *max_slope_filters[i]);
      if (i == 0 &&
          max_slope_filters[1] == &range_filter_cost_info[0])
        max_slope_filters[1]= max_slope_filters[0];

      best_filter_count++;
      max_slope_filters[i]= &range_filter_cost_info[i];
    }
  }
  sort_range_filter_cost_info_array();
}


void TABLE::select_usable_range_filters(THD *thd)
{
  uint key_no;
  key_map usable_range_filter_keys;
  usable_range_filter_keys.clear_all();
  key_map::Iterator it(quick_keys);
  while ((key_no= it++) != key_map::Iterator::BITMAP_END)
  {
    if (quick_rows[key_no] >
        thd->variables.max_rowid_filter_size/file->ref_length)
      continue;
    usable_range_filter_keys.set_bit(key_no);
  }

  if (usable_range_filter_keys.is_clear_all())
    return;

  range_filter_cost_info_elements= usable_range_filter_keys.bits_set();
  range_filter_cost_info=
    new (thd->mem_root) Range_filter_cost_info [range_filter_cost_info_elements];
  Range_filter_cost_info *curr_filter_cost_info= range_filter_cost_info;

  key_map::Iterator li(usable_range_filter_keys);
  while ((key_no= li++) != key_map::Iterator::BITMAP_END)
  {
    curr_filter_cost_info->init(this, key_no);
    curr_filter_cost_info++;
  }
  prune_range_filters();
}


Range_filter_cost_info
*TABLE::best_filter_for_current_join_order(uint ref_key_no,
                                           double record_count,
                                           double records)
{
  if (!this || range_filter_cost_info_elements == 0)
    return 0;

  double card= record_count*records;
  Range_filter_cost_info *best_filter= &range_filter_cost_info[0];

  if (card < best_filter->intersect_x_axis_abcissa)
    return 0;
  if (best_filter_count != 0)
  {
    if (best_filter->key_no == ref_key_no)
    {
      if (best_filter_count == 2)
      {
        best_filter= &range_filter_cost_info[1];
        if (card < best_filter->intersect_x_axis_abcissa)
          return 0;
        return best_filter;
      }
    }
    else
      return best_filter;
  }

  double best_filter_improvement= 0.0;
  best_filter= 0;

  key_map *intersected_with= &key_info->intersected_with;
  for (uint i= best_filter_count; i < range_filter_cost_info_elements; i++)
  {
    Range_filter_cost_info *filter= &range_filter_cost_info[i];
    if ((filter->key_no == ref_key_no) || intersected_with->is_set(filter->key_no))
      continue;
    if (card < filter->intersect_x_axis_abcissa)
      break;
    if (best_filter_improvement < filter->get_filter_gain(card))
    {
      best_filter_improvement= filter->get_filter_gain(card);
      best_filter= filter;
    }
  }
  return best_filter;
}


bool Range_filter_ordered_array::fill()
{
  int rc= 0;
  handler *file= table->file;
  THD *thd= table->in_use;
  QUICK_RANGE_SELECT* quick= (QUICK_RANGE_SELECT*) select->quick;
  uint table_status_save= table->status;
  Item *pushed_idx_cond_save= file->pushed_idx_cond;
  uint pushed_idx_cond_keyno_save= file->pushed_idx_cond_keyno;
  bool in_range_check_pushed_down_save= file->in_range_check_pushed_down;

  table->status= 0;
  file->pushed_idx_cond= 0;
  file->pushed_idx_cond_keyno= MAX_KEY;
  file->in_range_check_pushed_down= false;

  /* We're going to just read rowids. */
  table->prepare_for_position();

  table->file->ha_start_keyread(quick->index);

  if (quick->init() || quick->reset())
    rc= 1;

  while (!rc)
  {
    rc= quick->get_next();
    if (thd->killed)
      rc= 1;
    if (!rc)
    {
      file->position(quick->record);
      if (refpos_container.add((char*) file->ref))
        rc= 1;
    }
  }

  quick->range_end();
  table->file->ha_end_keyread();
  table->status= table_status_save;
  file->pushed_idx_cond= pushed_idx_cond_save;
  file->pushed_idx_cond_keyno= pushed_idx_cond_keyno_save;
  file->in_range_check_pushed_down= in_range_check_pushed_down_save;
  if (rc != HA_ERR_END_OF_FILE)
    return 1;
  container_is_filled= true;
  table->file->rowid_filter_is_active= true;
  return 0;
}


bool Range_filter_ordered_array::sort()
{
  refpos_container.sort(refpos_order_cmp, (void *) (table->file));
  return false;
}


bool Range_filter_ordered_array::check(char *elem)
{
  int l= 0;
  int r= refpos_container.elements()-1;
  while (l <= r)
  {
    int m= (l + r) / 2;
    int cmp= refpos_order_cmp((void *) (table->file),
                              refpos_container.get_pos(m), elem);
    if (cmp == 0)
      return true;
    if (cmp < 0)
      l= m + 1;
    else
      r= m-1;
  }
  return false;
}


Range_filter_ordered_array::~Range_filter_ordered_array()
{
  if (select)
  {
    if (select->quick)
    {
      delete select->quick;
      select->quick= 0;
    }
    delete select;
    select= 0;
  }
}
