/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/mask_select.h
 * Copyright (C) 2016 William Wilgus
 * Derivative of folder_select.h by:
 * Copyright (C) 2011 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Interface to mask_select.c.
 ****************************************************************************/

#ifndef __MASK_SELECT_H__
#define __MASK_SELECT_H__

/**
 * A GUI browser to select masks on a target
 *
 * It reads an original mask supplied to function
 * and pre-selects the corresponding actions in the UI. If the user is done  it
 * returns the new mask, assuming the user confirms the yesno dialog.
 *
 * Returns new mask if the selected options have changed, otherwise
 * returns the mask originally supplied */
struct s_mask_items {
    const char* name;
    const int bit_value;
};
int mask_select(int mask, const unsigned char* headermsg,
                             struct s_mask_items *mask_items, size_t items);
#endif /* __MASK_SELECT_H__ */
