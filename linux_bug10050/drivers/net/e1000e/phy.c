/*******************************************************************************

  Intel PRO/1000 Linux driver
  Copyright(c) 1999 - 2007 Intel Corporation.

  This program is free software; you can redistribute it and/or modify it
  under the terms and conditions of the GNU General Public License,
  version 2, as published by the Free Software Foundation.

  This program is distributed in the hope it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
  more details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.

  The full GNU General Public License is included in this distribution in
  the file called "COPYING".

  Contact Information:
  Linux NICS <linux.nics@intel.com>
  e1000-devel Mailing List <e1000-devel@lists.sourceforge.net>
  Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497

*******************************************************************************/

#include <linux/delay.h>

#include "e1000.h"

static s32 e1000_get_phy_cfg_done(struct e1000_hw *hw);
static s32 e1000_phy_force_speed_duplex(struct e1000_hw *hw);
static s32 e1000_set_d0_lplu_state(struct e1000_hw *hw, bool active);
static s32 e1000_wait_autoneg(struct e1000_hw *hw);

/* Cable length tables */
static const u16 e1000_m88_cable_length_table[] =
	{ 0, 50, 80, 110, 140, 140, E1000_CABLE_LENGTH_UNDEFINED };

static const u16 e1000_igp_2_cable_length_table[] =
	{ 0, 0, 0, 0, 0, 0, 0, 0, 3, 5, 8, 11, 13, 16, 18, 21, 0, 0, 0, 3,
	  6, 10, 13, 16, 19, 23, 26, 29, 32, 35, 38, 41, 6, 10, 14, 18, 22,
	  26, 30, 33, 37, 41, 44, 48, 51, 54, 58, 61, 21, 26, 31, 35, 40,
	  44, 49, 53, 57, 61, 65, 68, 72, 75, 79, 82, 40, 45, 51, 56, 61,
	  66, 70, 75, 79, 83, 87, 91, 94, 98, 101, 104, 60, 66, 72, 77, 82,
	  87, 92, 96, 100, 104, 108, 111, 114, 117, 119, 121, 83, 89, 95,
	  100, 105, 109, 113, 116, 119, 122, 124, 104, 109, 114, 118, 121,
	  124};
#define IGP02E1000_CABLE_LENGTH_TABLE_SIZE \
		ARRAY_SIZE(e1000_igp_2_cable_length_table)

/**
 *  e1000e_check_reset_block_generic - Check if PHY reset is blocked
 *  @hw: pointer to the HW structure
 *
 *  Read the PHY management control register and check whether a PHY reset
 *  is blocked.  If a reset is not blocked return 0, otherwise
 *  return E1000_BLK_PHY_RESET (12).
 **/
s32 e1000e_check_reset_block_generic(struct e1000_hw *hw)
{
	u32 manc;

	manc = er32(MANC);

	return (manc & E1000_MANC_BLK_PHY_RST_ON_IDE) ?
	       E1000_BLK_PHY_RESET : 0;
}

/**
 *  e1000e_get_phy_id - Retrieve the PHY ID and revision
 *  @hw: pointer to the HW structure
 *
 *  Reads the PHY registers and stores the PHY ID and possibly the PHY
 *  revision in the hardware structure.
 **/
s32 e1000e_get_phy_id(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 phy_id;

	ret_val = e1e_rphy(hw, PHY_ID1, &phy_id);
	if (ret_val)
		return ret_val;

	phy->id = (u32)(phy_id << 16);
	udelay(20);
	ret_val = e1e_rphy(hw, PHY_ID2, &phy_id);
	if (ret_val)
		return ret_val;

	phy->id |= (u32)(phy_id & PHY_REVISION_MASK);
	phy->revision = (u32)(phy_id & ~PHY_REVISION_MASK);

	return 0;
}

/**
 *  e1000e_phy_reset_dsp - Reset PHY DSP
 *  @hw: pointer to the HW structure
 *
 *  Reset the digital signal processor.
 **/
s32 e1000e_phy_reset_dsp(struct e1000_hw *hw)
{
	s32 ret_val;

	ret_val = e1e_wphy(hw, M88E1000_PHY_GEN_CONTROL, 0xC1);
	if (ret_val)
		return ret_val;

	return e1e_wphy(hw, M88E1000_PHY_GEN_CONTROL, 0);
}

/**
 *  e1000_read_phy_reg_mdic - Read MDI control register
 *  @hw: pointer to the HW structure
 *  @offset: register offset to be read
 *  @data: pointer to the read data
 *
 *  Reads the MDI control regsiter in the PHY at offset and stores the
 *  information read to data.
 **/
static s32 e1000_read_phy_reg_mdic(struct e1000_hw *hw, u32 offset, u16 *data)
{
	struct e1000_phy_info *phy = &hw->phy;
	u32 i, mdic = 0;

	if (offset > MAX_PHY_REG_ADDRESS) {
		hw_dbg(hw, "PHY Address %d is out of range\n", offset);
		return -E1000_ERR_PARAM;
	}

	/* Set up Op-code, Phy Address, and register offset in the MDI
	 * Control register.  The MAC will take care of interfacing with the
	 * PHY to retrieve the desired data.
	 */
	mdic = ((offset << E1000_MDIC_REG_SHIFT) |
		(phy->addr << E1000_MDIC_PHY_SHIFT) |
		(E1000_MDIC_OP_READ));

	ew32(MDIC, mdic);

	/* Poll the ready bit to see if the MDI read completed */
	for (i = 0; i < 64; i++) {
		udelay(50);
		mdic = er32(MDIC);
		if (mdic & E1000_MDIC_READY)
			break;
	}
	if (!(mdic & E1000_MDIC_READY)) {
		hw_dbg(hw, "MDI Read did not complete\n");
		return -E1000_ERR_PHY;
	}
	if (mdic & E1000_MDIC_ERROR) {
		hw_dbg(hw, "MDI Error\n");
		return -E1000_ERR_PHY;
	}
	*data = (u16) mdic;

	return 0;
}

/**
 *  e1000_write_phy_reg_mdic - Write MDI control register
 *  @hw: pointer to the HW structure
 *  @offset: register offset to write to
 *  @data: data to write to register at offset
 *
 *  Writes data to MDI control register in the PHY at offset.
 **/
static s32 e1000_write_phy_reg_mdic(struct e1000_hw *hw, u32 offset, u16 data)
{
	struct e1000_phy_info *phy = &hw->phy;
	u32 i, mdic = 0;

	if (offset > MAX_PHY_REG_ADDRESS) {
		hw_dbg(hw, "PHY Address %d is out of range\n", offset);
		return -E1000_ERR_PARAM;
	}

	/* Set up Op-code, Phy Address, and register offset in the MDI
	 * Control register.  The MAC will take care of interfacing with the
	 * PHY to retrieve the desired data.
	 */
	mdic = (((u32)data) |
		(offset << E1000_MDIC_REG_SHIFT) |
		(phy->addr << E1000_MDIC_PHY_SHIFT) |
		(E1000_MDIC_OP_WRITE));

	ew32(MDIC, mdic);

	/* Poll the ready bit to see if the MDI read completed */
	for (i = 0; i < E1000_GEN_POLL_TIMEOUT; i++) {
		udelay(5);
		mdic = er32(MDIC);
		if (mdic & E1000_MDIC_READY)
			break;
	}
	if (!(mdic & E1000_MDIC_READY)) {
		hw_dbg(hw, "MDI Write did not complete\n");
		return -E1000_ERR_PHY;
	}

	return 0;
}

/**
 *  e1000e_read_phy_reg_m88 - Read m88 PHY register
 *  @hw: pointer to the HW structure
 *  @offset: register offset to be read
 *  @data: pointer to the read data
 *
 *  Acquires semaphore, if necessary, then reads the PHY register at offset
 *  and storing the retrieved information in data.  Release any acquired
 *  semaphores before exiting.
 **/
s32 e1000e_read_phy_reg_m88(struct e1000_hw *hw, u32 offset, u16 *data)
{
	s32 ret_val;

	ret_val = hw->phy.ops.acquire_phy(hw);
	if (ret_val)
		return ret_val;

	ret_val = e1000_read_phy_reg_mdic(hw,
					  MAX_PHY_REG_ADDRESS & offset,
					  data);

	hw->phy.ops.release_phy(hw);

	return ret_val;
}

/**
 *  e1000e_write_phy_reg_m88 - Write m88 PHY register
 *  @hw: pointer to the HW structure
 *  @offset: register offset to write to
 *  @data: data to write at register offset
 *
 *  Acquires semaphore, if necessary, then writes the data to PHY register
 *  at the offset.  Release any acquired semaphores before exiting.
 **/
s32 e1000e_write_phy_reg_m88(struct e1000_hw *hw, u32 offset, u16 data)
{
	s32 ret_val;

	ret_val = hw->phy.ops.acquire_phy(hw);
	if (ret_val)
		return ret_val;

	ret_val = e1000_write_phy_reg_mdic(hw,
					   MAX_PHY_REG_ADDRESS & offset,
					   data);

	hw->phy.ops.release_phy(hw);

	return ret_val;
}

/**
 *  e1000e_read_phy_reg_igp - Read igp PHY register
 *  @hw: pointer to the HW structure
 *  @offset: register offset to be read
 *  @data: pointer to the read data
 *
 *  Acquires semaphore, if necessary, then reads the PHY register at offset
 *  and storing the retrieved information in data.  Release any acquired
 *  semaphores before exiting.
 **/
s32 e1000e_read_phy_reg_igp(struct e1000_hw *hw, u32 offset, u16 *data)
{
	s32 ret_val;

	ret_val = hw->phy.ops.acquire_phy(hw);
	if (ret_val)
		return ret_val;

	if (offset > MAX_PHY_MULTI_PAGE_REG) {
		ret_val = e1000_write_phy_reg_mdic(hw,
						   IGP01E1000_PHY_PAGE_SELECT,
						   (u16)offset);
		if (ret_val) {
			hw->phy.ops.release_phy(hw);
			return ret_val;
		}
	}

	ret_val = e1000_read_phy_reg_mdic(hw,
					  MAX_PHY_REG_ADDRESS & offset,
					  data);

	hw->phy.ops.release_phy(hw);

	return ret_val;
}

/**
 *  e1000e_write_phy_reg_igp - Write igp PHY register
 *  @hw: pointer to the HW structure
 *  @offset: register offset to write to
 *  @data: data to write at register offset
 *
 *  Acquires semaphore, if necessary, then writes the data to PHY register
 *  at the offset.  Release any acquired semaphores before exiting.
 **/
s32 e1000e_write_phy_reg_igp(struct e1000_hw *hw, u32 offset, u16 data)
{
	s32 ret_val;

	ret_val = hw->phy.ops.acquire_phy(hw);
	if (ret_val)
		return ret_val;

	if (offset > MAX_PHY_MULTI_PAGE_REG) {
		ret_val = e1000_write_phy_reg_mdic(hw,
						   IGP01E1000_PHY_PAGE_SELECT,
						   (u16)offset);
		if (ret_val) {
			hw->phy.ops.release_phy(hw);
			return ret_val;
		}
	}

	ret_val = e1000_write_phy_reg_mdic(hw,
					   MAX_PHY_REG_ADDRESS & offset,
					   data);

	hw->phy.ops.release_phy(hw);

	return ret_val;
}

/**
 *  e1000e_read_kmrn_reg - Read kumeran register
 *  @hw: pointer to the HW structure
 *  @offset: register offset to be read
 *  @data: pointer to the read data
 *
 *  Acquires semaphore, if necessary.  Then reads the PHY register at offset
 *  using the kumeran interface.  The information retrieved is stored in data.
 *  Release any acquired semaphores before exiting.
 **/
s32 e1000e_read_kmrn_reg(struct e1000_hw *hw, u32 offset, u16 *data)
{
	u32 kmrnctrlsta;
	s32 ret_val;

	ret_val = hw->phy.ops.acquire_phy(hw);
	if (ret_val)
		return ret_val;

	kmrnctrlsta = ((offset << E1000_KMRNCTRLSTA_OFFSET_SHIFT) &
		       E1000_KMRNCTRLSTA_OFFSET) | E1000_KMRNCTRLSTA_REN;
	ew32(KMRNCTRLSTA, kmrnctrlsta);

	udelay(2);

	kmrnctrlsta = er32(KMRNCTRLSTA);
	*data = (u16)kmrnctrlsta;

	hw->phy.ops.release_phy(hw);

	return ret_val;
}

/**
 *  e1000e_write_kmrn_reg - Write kumeran register
 *  @hw: pointer to the HW structure
 *  @offset: register offset to write to
 *  @data: data to write at register offset
 *
 *  Acquires semaphore, if necessary.  Then write the data to PHY register
 *  at the offset using the kumeran interface.  Release any acquired semaphores
 *  before exiting.
 **/
s32 e1000e_write_kmrn_reg(struct e1000_hw *hw, u32 offset, u16 data)
{
	u32 kmrnctrlsta;
	s32 ret_val;

	ret_val = hw->phy.ops.acquire_phy(hw);
	if (ret_val)
		return ret_val;

	kmrnctrlsta = ((offset << E1000_KMRNCTRLSTA_OFFSET_SHIFT) &
		       E1000_KMRNCTRLSTA_OFFSET) | data;
	ew32(KMRNCTRLSTA, kmrnctrlsta);

	udelay(2);
	hw->phy.ops.release_phy(hw);

	return ret_val;
}

/**
 *  e1000e_copper_link_setup_m88 - Setup m88 PHY's for copper link
 *  @hw: pointer to the HW structure
 *
 *  Sets up MDI/MDI-X and polarity for m88 PHY's.  If necessary, transmit clock
 *  and downshift values are set also.
 **/
s32 e1000e_copper_link_setup_m88(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 phy_data;

	/* Enable CRS on TX. This must be set for half-duplex operation. */
	ret_val = e1e_rphy(hw, M88E1000_PHY_SPEC_CTRL, &phy_data);
	if (ret_val)
		return ret_val;

	phy_data |= M88E1000_PSCR_ASSERT_CRS_ON_TX;

	/* Options:
	 *   MDI/MDI-X = 0 (default)
	 *   0 - Auto for all speeds
	 *   1 - MDI mode
	 *   2 - MDI-X mode
	 *   3 - Auto for 1000Base-T only (MDI-X for 10/100Base-T modes)
	 */
	phy_data &= ~M88E1000_PSCR_AUTO_X_MODE;

	switch (phy->mdix) {
	case 1:
		phy_data |= M88E1000_PSCR_MDI_MANUAL_MODE;
		break;
	case 2:
		phy_data |= M88E1000_PSCR_MDIX_MANUAL_MODE;
		break;
	case 3:
		phy_data |= M88E1000_PSCR_AUTO_X_1000T;
		break;
	case 0:
	default:
		phy_data |= M88E1000_PSCR_AUTO_X_MODE;
		break;
	}

	/* Options:
	 *   disable_polarity_correction = 0 (default)
	 *       Automatic Correction for Reversed Cable Polarity
	 *   0 - Disabled
	 *   1 - Enabled
	 */
	phy_data &= ~M88E1000_PSCR_POLARITY_REVERSAL;
	if (phy->disable_polarity_correction == 1)
		phy_data |= M88E1000_PSCR_POLARITY_REVERSAL;

	ret_val = e1e_wphy(hw, M88E1000_PHY_SPEC_CTRL, phy_data);
	if (ret_val)
		return ret_val;

	if (phy->revision < 4) {
		/* Force TX_CLK in the Extended PHY Specific Control Register
		 * to 25MHz clock.
		 */
		ret_val = e1e_rphy(hw, M88E1000_EXT_PHY_SPEC_CTRL, &phy_data);
		if (ret_val)
			return ret_val;

		phy_data |= M88E1000_EPSCR_TX_CLK_25;

		if ((phy->revision == 2) &&
		    (phy->id == M88E1111_I_PHY_ID)) {
			/* 82573L PHY - set the downshift counter to 5x. */
			phy_data &= ~M88EC018_EPSCR_DOWNSHIFT_COUNTER_MASK;
			phy_data |= M88EC018_EPSCR_DOWNSHIFT_COUNTER_5X;
		} else {
			/* Configure Master and Slave downshift values */
			phy_data &= ~(M88E1000_EPSCR_MASTER_DOWNSHIFT_MASK |
				      M88E1000_EPSCR_SLAVE_DOWNSHIFT_MASK);
			phy_data |= (M88E1000_EPSCR_MASTER_DOWNSHIFT_1X |
				     M88E1000_EPSCR_SLAVE_DOWNSHIFT_1X);
		}
		ret_val = e1e_wphy(hw, M88E1000_EXT_PHY_SPEC_CTRL, phy_data);
		if (ret_val)
			return ret_val;
	}

	/* Commit the changes. */
	ret_val = e1000e_commit_phy(hw);
	if (ret_val)
		hw_dbg(hw, "Error committing the PHY changes\n");

	return ret_val;
}

/**
 *  e1000e_copper_link_setup_igp - Setup igp PHY's for copper link
 *  @hw: pointer to the HW structure
 *
 *  Sets up LPLU, MDI/MDI-X, polarity, Smartspeed and Master/Slave config for
 *  igp PHY's.
 **/
s32 e1000e_copper_link_setup_igp(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 data;

	ret_val = e1000_phy_hw_reset(hw);
	if (ret_val) {
		hw_dbg(hw, "Error resetting the PHY.\n");
		return ret_val;
	}

	/* Wait 15ms for MAC to configure PHY from NVM settings. */
	msleep(15);

	/* disable lplu d0 during driver init */
	ret_val = e1000_set_d0_lplu_state(hw, 0);
	if (ret_val) {
		hw_dbg(hw, "Error Disabling LPLU D0\n");
		return ret_val;
	}
	/* Configure mdi-mdix settings */
	ret_val = e1e_rphy(hw, IGP01E1000_PHY_PORT_CTRL, &data);
	if (ret_val)
		return ret_val;

	data &= ~IGP01E1000_PSCR_AUTO_MDIX;

	switch (phy->mdix) {
	case 1:
		data &= ~IGP01E1000_PSCR_FORCE_MDI_MDIX;
		break;
	case 2:
		data |= IGP01E1000_PSCR_FORCE_MDI_MDIX;
		break;
	case 0:
	default:
		data |= IGP01E1000_PSCR_AUTO_MDIX;
		break;
	}
	ret_val = e1e_wphy(hw, IGP01E1000_PHY_PORT_CTRL, data);
	if (ret_val)
		return ret_val;

	/* set auto-master slave resolution settings */
	if (hw->mac.autoneg) {
		/* when autonegotiation advertisement is only 1000Mbps then we
		 * should disable SmartSpeed and enable Auto MasterSlave
		 * resolution as hardware default. */
		if (phy->autoneg_advertised == ADVERTISE_1000_FULL) {
			/* Disable SmartSpeed */
			ret_val = e1e_rphy(hw, IGP01E1000_PHY_PORT_CONFIG,
						     &data);
			if (ret_val)
				return ret_val;

			data &= ~IGP01E1000_PSCFR_SMART_SPEED;
			ret_val = e1e_wphy(hw, IGP01E1000_PHY_PORT_CONFIG,
						     data);
			if (ret_val)
				return ret_val;

			/* Set auto Master/Slave resolution process */
			ret_val = e1e_rphy(hw, PHY_1000T_CTRL, &data);
			if (ret_val)
				return ret_val;

			data &= ~CR_1000T_MS_ENABLE;
			ret_val = e1e_wphy(hw, PHY_1000T_CTRL, data);
			if (ret_val)
				return ret_val;
		}

		ret_val = e1e_rphy(hw, PHY_1000T_CTRL, &data);
		if (ret_val)
			return ret_val;

		/* load defaults for future use */
		phy->original_ms_type = (data & CR_1000T_MS_ENABLE) ?
			((data & CR_1000T_MS_VALUE) ?
			e1000_ms_force_master :
			e1000_ms_force_slave) :
			e1000_ms_auto;

		switch (phy->ms_type) {
		case e1000_ms_force_master:
			data |= (CR_1000T_MS_ENABLE | CR_1000T_MS_VALUE);
			break;
		case e1000_ms_force_slave:
			data |= CR_1000T_MS_ENABLE;
			data &= ~(CR_1000T_MS_VALUE);
			break;
		case e1000_ms_auto:
			data &= ~CR_1000T_MS_ENABLE;
		default:
			break;
		}
		ret_val = e1e_wphy(hw, PHY_1000T_CTRL, data);
	}

	return ret_val;
}

/**
 *  e1000_phy_setup_autoneg - Configure PHY for auto-negotiation
 *  @hw: pointer to the HW structure
 *
 *  Reads the MII auto-neg advertisement register and/or the 1000T control
 *  register and if the PHY is already setup for auto-negotiation, then
 *  return successful.  Otherwise, setup advertisement and flow control to
 *  the appropriate values for the wanted auto-negotiation.
 **/
static s32 e1000_phy_setup_autoneg(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 mii_autoneg_adv_reg;
	u16 mii_1000t_ctrl_reg = 0;

	phy->autoneg_advertised &= phy->autoneg_mask;

	/* Read the MII Auto-Neg Advertisement Register (Address 4). */
	ret_val = e1e_rphy(hw, PHY_AUTONEG_ADV, &mii_autoneg_adv_reg);
	if (ret_val)
		return ret_val;

	if (phy->autoneg_mask & ADVERTISE_1000_FULL) {
		/* Read the MII 1000Base-T Control Register (Address 9). */
		ret_val = e1e_rphy(hw, PHY_1000T_CTRL, &mii_1000t_ctrl_reg);
		if (ret_val)
			return ret_val;
	}

	/* Need to parse both autoneg_advertised and fc and set up
	 * the appropriate PHY registers.  First we will parse for
	 * autoneg_advertised software override.  Since we can advertise
	 * a plethora of combinations, we need to check each bit
	 * individually.
	 */

	/* First we clear all the 10/100 mb speed bits in the Auto-Neg
	 * Advertisement Register (Address 4) and the 1000 mb speed bits in
	 * the  1000Base-T Control Register (Address 9).
	 */
	mii_autoneg_adv_reg &= ~(NWAY_AR_100TX_FD_CAPS |
				 NWAY_AR_100TX_HD_CAPS |
				 NWAY_AR_10T_FD_CAPS   |
				 NWAY_AR_10T_HD_CAPS);
	mii_1000t_ctrl_reg &= ~(CR_1000T_HD_CAPS | CR_1000T_FD_CAPS);

	hw_dbg(hw, "autoneg_advertised %x\n", phy->autoneg_advertised);

	/* Do we want to advertise 10 Mb Half Duplex? */
	if (phy->autoneg_advertised & ADVERTISE_10_HALF) {
		hw_dbg(hw, "Advertise 10mb Half duplex\n");
		mii_autoneg_adv_reg |= NWAY_AR_10T_HD_CAPS;
	}

	/* Do we want to advertise 10 Mb Full Duplex? */
	if (phy->autoneg_advertised & ADVERTISE_10_FULL) {
		hw_dbg(hw, "Advertise 10mb Full duplex\n");
		mii_autoneg_adv_reg |= NWAY_AR_10T_FD_CAPS;
	}

	/* Do we want to advertise 100 Mb Half Duplex? */
	if (phy->autoneg_advertised & ADVERTISE_100_HALF) {
		hw_dbg(hw, "Advertise 100mb Half duplex\n");
		mii_autoneg_adv_reg |= NWAY_AR_100TX_HD_CAPS;
	}

	/* Do we want to advertise 100 Mb Full Duplex? */
	if (phy->autoneg_advertised & ADVERTISE_100_FULL) {
		hw_dbg(hw, "Advertise 100mb Full duplex\n");
		mii_autoneg_adv_reg |= NWAY_AR_100TX_FD_CAPS;
	}

	/* We do not allow the Phy to advertise 1000 Mb Half Duplex */
	if (phy->autoneg_advertised & ADVERTISE_1000_HALF)
		hw_dbg(hw, "Advertise 1000mb Half duplex request denied!\n");

	/* Do we want to advertise 1000 Mb Full Duplex? */
	if (phy->autoneg_advertised & ADVERTISE_1000_FULL) {
		hw_dbg(hw, "Advertise 1000mb Full duplex\n");
		mii_1000t_ctrl_reg |= CR_1000T_FD_CAPS;
	}

	/* Check for a software override of the flow control settings, and
	 * setup the PHY advertisement registers accordingly.  If
	 * auto-negotiation is enabled, then software will have to set the
	 * "PAUSE" bits to the correct value in the Auto-Negotiation
	 * Advertisement Register (PHY_AUTONEG_ADV) and re-start auto-
	 * negotiation.
	 *
	 * The possible values of the "fc" parameter are:
	 *      0:  Flow control is completely disabled
	 *      1:  Rx flow control is enabled (we can receive pause frames
	 *	  but not send pause frames).
	 *      2:  Tx flow control is enabled (we can send pause frames
	 *	  but we do not support receiving pause frames).
	 *      3:  Both Rx and TX flow control (symmetric) are enabled.
	 *  other:  No software override.  The flow control configuration
	 *	  in the EEPROM is used.
	 */
	switch (hw->mac.fc) {
	case e1000_fc_none:
		/* Flow control (RX & TX) is completely disabled by a
		 * software over-ride.
		 */
		mii_autoneg_adv_reg &= ~(NWAY_AR_ASM_DIR | NWAY_AR_PAUSE);
		break;
	case e1000_fc_rx_pause:
		/* RX Flow control is enabled, and TX Flow control is
		 * disabled, by a software over-ride.
		 */
		/* Since there really isn't a way to advertise that we are
		 * capable of RX Pause ONLY, we will advertise that we
		 * support both symmetric and asymmetric RX PAUSE.  Later
		 * (in e1000e_config_fc_after_link_up) we will disable the
		 * hw's ability to send PAUSE frames.
		 */
		mii_autoneg_adv_reg |= (NWAY_AR_ASM_DIR | NWAY_AR_PAUSE);
		break;
	case e1000_fc_tx_pause:
		/* TX Flow control is enabled, and RX Flow control is
		 * disabled, by a software over-ride.
		 */
		mii_autoneg_adv_reg |= NWAY_AR_ASM_DIR;
		mii_autoneg_adv_reg &= ~NWAY_AR_PAUSE;
		break;
	case e1000_fc_full:
		/* Flow control (both RX and TX) is enabled by a software
		 * over-ride.
		 */
		mii_autoneg_adv_reg |= (NWAY_AR_ASM_DIR | NWAY_AR_PAUSE);
		break;
	default:
		hw_dbg(hw, "Flow control param set incorrectly\n");
		ret_val = -E1000_ERR_CONFIG;
		return ret_val;
	}

	ret_val = e1e_wphy(hw, PHY_AUTONEG_ADV, mii_autoneg_adv_reg);
	if (ret_val)
		return ret_val;

	hw_dbg(hw, "Auto-Neg Advertising %x\n", mii_autoneg_adv_reg);

	if (phy->autoneg_mask & ADVERTISE_1000_FULL) {
		ret_val = e1e_wphy(hw, PHY_1000T_CTRL, mii_1000t_ctrl_reg);
	}

	return ret_val;
}

/**
 *  e1000_copper_link_autoneg - Setup/Enable autoneg for copper link
 *  @hw: pointer to the HW structure
 *
 *  Performs initial bounds checking on autoneg advertisement parameter, then
 *  configure to advertise the full capability.  Setup the PHY to autoneg
 *  and restart the negotiation process between the link partner.  If
 *  wait_for_link, then wait for autoneg to complete before exiting.
 **/
static s32 e1000_copper_link_autoneg(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 phy_ctrl;

	/* Perform some bounds checking on the autoneg advertisement
	 * parameter.
	 */
	phy->autoneg_advertised &= phy->autoneg_mask;

	/* If autoneg_advertised is zero, we assume it was not defaulted
	 * by the calling code so we set to advertise full capability.
	 */
	if (phy->autoneg_advertised == 0)
		phy->autoneg_advertised = phy->autoneg_mask;

	hw_dbg(hw, "Reconfiguring auto-neg advertisement params\n");
	ret_val = e1000_phy_setup_autoneg(hw);
	if (ret_val) {
		hw_dbg(hw, "Error Setting up Auto-Negotiation\n");
		return ret_val;
	}
	hw_dbg(hw, "Restarting Auto-Neg\n");

	/* Restart auto-negotiation by setting the Auto Neg Enable bit and
	 * the Auto Neg Restart bit in the PHY control register.
	 */
	ret_val = e1e_rphy(hw, PHY_CONTROL, &phy_ctrl);
	if (ret_val)
		return ret_val;

	phy_ctrl |= (MII_CR_AUTO_NEG_EN | MII_CR_RESTART_AUTO_NEG);
	ret_val = e1e_wphy(hw, PHY_CONTROL, phy_ctrl);
	if (ret_val)
		return ret_val;

	/* Does the user want to wait for Auto-Neg to complete here, or
	 * check at a later time (for example, callback routine).
	 */
	if (phy->wait_for_link) {
		ret_val = e1000_wait_autoneg(hw);
		if (ret_val) {
			hw_dbg(hw, "Error while waiting for "
				 "autoneg to complete\n");
			return ret_val;
		}
	}

	hw->mac.get_link_status = 1;

	return ret_val;
}

/**
 *  e1000e_setup_copper_link - Configure copper link settings
 *  @hw: pointer to the HW structure
 *
 *  Calls the appropriate function to configure the link for auto-neg or forced
 *  speed and duplex.  Then we check for link, once link is established calls
 *  to configure collision distance and flow control are called.  If link is
 *  not established, we return -E1000_ERR_PHY (-2).
 **/
s32 e1000e_setup_copper_link(struct e1000_hw *hw)
{
	s32 ret_val;
	bool link;

	if (hw->mac.autoneg) {
		/* Setup autoneg and flow control advertisement and perform
		 * autonegotiation. */
		ret_val = e1000_copper_link_autoneg(hw);
		if (ret_val)
			return ret_val;
	} else {
		/* PHY will be set to 10H, 10F, 100H or 100F
		 * depending on user settings. */
		hw_dbg(hw, "Forcing Speed and Duplex\n");
		ret_val = e1000_phy_force_speed_duplex(hw);
		if (ret_val) {
			hw_dbg(hw, "Error Forcing Speed and Duplex\n");
			return ret_val;
		}
	}

	/* Check link status. Wait up to 100 microseconds for link to become
	 * valid.
	 */
	ret_val = e1000e_phy_has_link_generic(hw,
					     COPPER_LINK_UP_LIMIT,
					     10,
					     &link);
	if (ret_val)
		return ret_val;

	if (link) {
		hw_dbg(hw, "Valid link established!!!\n");
		e1000e_config_collision_dist(hw);
		ret_val = e1000e_config_fc_after_link_up(hw);
	} else {
		hw_dbg(hw, "Unable to establish link!!!\n");
	}

	return ret_val;
}

/**
 *  e1000e_phy_force_speed_duplex_igp - Force speed/duplex for igp PHY
 *  @hw: pointer to the HW structure
 *
 *  Calls the PHY setup function to force speed and duplex.  Clears the
 *  auto-crossover to force MDI manually.  Waits for link and returns
 *  successful if link up is successful, else -E1000_ERR_PHY (-2).
 **/
s32 e1000e_phy_force_speed_duplex_igp(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 phy_data;
	bool link;

	ret_val = e1e_rphy(hw, PHY_CONTROL, &phy_data);
	if (ret_val)
		return ret_val;

	e1000e_phy_force_speed_duplex_setup(hw, &phy_data);

	ret_val = e1e_wphy(hw, PHY_CONTROL, phy_data);
	if (ret_val)
		return ret_val;

	/* Clear Auto-Crossover to force MDI manually.  IGP requires MDI
	 * forced whenever speed and duplex are forced.
	 */
	ret_val = e1e_rphy(hw, IGP01E1000_PHY_PORT_CTRL, &phy_data);
	if (ret_val)
		return ret_val;

	phy_data &= ~IGP01E1000_PSCR_AUTO_MDIX;
	phy_data &= ~IGP01E1000_PSCR_FORCE_MDI_MDIX;

	ret_val = e1e_wphy(hw, IGP01E1000_PHY_PORT_CTRL, phy_data);
	if (ret_val)
		return ret_val;

	hw_dbg(hw, "IGP PSCR: %X\n", phy_data);

	udelay(1);

	if (phy->wait_for_link) {
		hw_dbg(hw, "Waiting for forced speed/duplex link on IGP phy.\n");

		ret_val = e1000e_phy_has_link_generic(hw,
						     PHY_FORCE_LIMIT,
						     100000,
						     &link);
		if (ret_val)
			return ret_val;

		if (!link)
			hw_dbg(hw, "Link taking longer than expected.\n");

		/* Try once more */
		ret_val = e1000e_phy_has_link_generic(hw,
						     PHY_FORCE_LIMIT,
						     100000,
						     &link);
		if (ret_val)
			return ret_val;
	}

	return ret_val;
}

/**
 *  e1000e_phy_force_speed_duplex_m88 - Force speed/duplex for m88 PHY
 *  @hw: pointer to the HW structure
 *
 *  Calls the PHY setup function to force speed and duplex.  Clears the
 *  auto-crossover to force MDI manually.  Resets the PHY to commit the
 *  changes.  If time expires while waiting for link up, we reset the DSP.
 *  After reset, TX_CLK and CRS on TX must be set.  Return successful upon
 *  successful completion, else return corresponding error code.
 **/
s32 e1000e_phy_force_speed_duplex_m88(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 phy_data;
	bool link;

	/* Clear Auto-Crossover to force MDI manually.  M88E1000 requires MDI
	 * forced whenever speed and duplex are forced.
	 */
	ret_val = e1e_rphy(hw, M88E1000_PHY_SPEC_CTRL, &phy_data);
	if (ret_val)
		return ret_val;

	phy_data &= ~M88E1000_PSCR_AUTO_X_MODE;
	ret_val = e1e_wphy(hw, M88E1000_PHY_SPEC_CTRL, phy_data);
	if (ret_val)
		return ret_val;

	hw_dbg(hw, "M88E1000 PSCR: %X\n", phy_data);

	ret_val = e1e_rphy(hw, PHY_CONTROL, &phy_data);
	if (ret_val)
		return ret_val;

	e1000e_phy_force_speed_duplex_setup(hw, &phy_data);

	/* Reset the phy to commit changes. */
	phy_data |= MII_CR_RESET;

	ret_val = e1e_wphy(hw, PHY_CONTROL, phy_data);
	if (ret_val)
		return ret_val;

	udelay(1);

	if (phy->wait_for_link) {
		hw_dbg(hw, "Waiting for forced speed/duplex link on M88 phy.\n");

		ret_val = e1000e_phy_has_link_generic(hw, PHY_FORCE_LIMIT,
						     100000, &link);
		if (ret_val)
			return ret_val;

		if (!link) {
			/* We didn't get link.
			 * Reset the DSP and cross our fingers.
			 */
			ret_val = e1e_wphy(hw, M88E1000_PHY_PAGE_SELECT, 0x001d);
			if (ret_val)
				return ret_val;
			ret_val = e1000e_phy_reset_dsp(hw);
			if (ret_val)
				return ret_val;
		}

		/* Try once more */
		ret_val = e1000e_phy_has_link_generic(hw, PHY_FORCE_LIMIT,
						     100000, &link);
		if (ret_val)
			return ret_val;
	}

	ret_val = e1e_rphy(hw, M88E1000_EXT_PHY_SPEC_CTRL, &phy_data);
	if (ret_val)
		return ret_val;

	/* Resetting the phy means we need to re-force TX_CLK in the
	 * Extended PHY Specific Control Register to 25MHz clock from
	 * the reset value of 2.5MHz.
	 */
	phy_data |= M88E1000_EPSCR_TX_CLK_25;
	ret_val = e1e_wphy(hw, M88E1000_EXT_PHY_SPEC_CTRL, phy_data);
	if (ret_val)
		return ret_val;

	/* In addition, we must re-enable CRS on Tx for both half and full
	 * duplex.
	 */
	ret_val = e1e_rphy(hw, M88E1000_PHY_SPEC_CTRL, &phy_data);
	if (ret_val)
		return ret_val;

	phy_data |= M88E1000_PSCR_ASSERT_CRS_ON_TX;
	ret_val = e1e_wphy(hw, M88E1000_PHY_SPEC_CTRL, phy_data);

	return ret_val;
}

/**
 *  e1000e_phy_force_speed_duplex_setup - Configure forced PHY speed/duplex
 *  @hw: pointer to the HW structure
 *  @phy_ctrl: pointer to current value of PHY_CONTROL
 *
 *  Forces speed and duplex on the PHY by doing the following: disable flow
 *  control, force speed/duplex on the MAC, disable auto speed detection,
 *  disable auto-negotiation, configure duplex, configure speed, configure
 *  the collision distance, write configuration to CTRL register.  The
 *  caller must write to the PHY_CONTROL register for these settings to
 *  take affect.
 **/
void e1000e_phy_force_speed_duplex_setup(struct e1000_hw *hw, u16 *phy_ctrl)
{
	struct e1000_mac_info *mac = &hw->mac;
	u32 ctrl;

	/* Turn off flow control when forcing speed/duplex */
	mac->fc = e1000_fc_none;

	/* Force speed/duplex on the mac */
	ctrl = er32(CTRL);
	ctrl |= (E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX);
	ctrl &= ~E1000_CTRL_SPD_SEL;

	/* Disable Auto Speed Detection */
	ctrl &= ~E1000_CTRL_ASDE;

	/* Disable autoneg on the phy */
	*phy_ctrl &= ~MII_CR_AUTO_NEG_EN;

	/* Forcing Full or Half Duplex? */
	if (mac->forced_speed_duplex & E1000_ALL_HALF_DUPLEX) {
		ctrl &= ~E1000_CTRL_FD;
		*phy_ctrl &= ~MII_CR_FULL_DUPLEX;
		hw_dbg(hw, "Half Duplex\n");
	} else {
		ctrl |= E1000_CTRL_FD;
		*phy_ctrl |= MII_CR_FULL_DUPLEX;
		hw_dbg(hw, "Full Duplex\n");
	}

	/* Forcing 10mb or 100mb? */
	if (mac->forced_speed_duplex & E1000_ALL_100_SPEED) {
		ctrl |= E1000_CTRL_SPD_100;
		*phy_ctrl |= MII_CR_SPEED_100;
		*phy_ctrl &= ~(MII_CR_SPEED_1000 | MII_CR_SPEED_10);
		hw_dbg(hw, "Forcing 100mb\n");
	} else {
		ctrl &= ~(E1000_CTRL_SPD_1000 | E1000_CTRL_SPD_100);
		*phy_ctrl |= MII_CR_SPEED_10;
		*phy_ctrl &= ~(MII_CR_SPEED_1000 | MII_CR_SPEED_100);
		hw_dbg(hw, "Forcing 10mb\n");
	}

	e1000e_config_collision_dist(hw);

	ew32(CTRL, ctrl);
}

/**
 *  e1000e_set_d3_lplu_state - Sets low power link up state for D3
 *  @hw: pointer to the HW structure
 *  @active: boolean used to enable/disable lplu
 *
 *  Success returns 0, Failure returns 1
 *
 *  The low power link up (lplu) state is set to the power management level D3
 *  and SmartSpeed is disabled when active is true, else clear lplu for D3
 *  and enable Smartspeed.  LPLU and Smartspeed are mutually exclusive.  LPLU
 *  is used during Dx states where the power conservation is most important.
 *  During driver activity, SmartSpeed should be enabled so performance is
 *  maintained.
 **/
s32 e1000e_set_d3_lplu_state(struct e1000_hw *hw, bool active)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 data;

	ret_val = e1e_rphy(hw, IGP02E1000_PHY_POWER_MGMT, &data);
	if (ret_val)
		return ret_val;

	if (!active) {
		data &= ~IGP02E1000_PM_D3_LPLU;
		ret_val = e1e_wphy(hw,
					     IGP02E1000_PHY_POWER_MGMT,
					     data);
		if (ret_val)
			return ret_val;
		/* LPLU and SmartSpeed are mutually exclusive.  LPLU is used
		 * during Dx states where the power conservation is most
		 * important.  During driver activity we should enable
		 * SmartSpeed, so performance is maintained. */
		if (phy->smart_speed == e1000_smart_speed_on) {
			ret_val = e1e_rphy(hw, IGP01E1000_PHY_PORT_CONFIG,
						    &data);
			if (ret_val)
				return ret_val;

			data |= IGP01E1000_PSCFR_SMART_SPEED;
			ret_val = e1e_wphy(hw, IGP01E1000_PHY_PORT_CONFIG,
						     data);
			if (ret_val)
				return ret_val;
		} else if (phy->smart_speed == e1000_smart_speed_off) {
			ret_val = e1e_rphy(hw, IGP01E1000_PHY_PORT_CONFIG,
						     &data);
			if (ret_val)
				return ret_val;

			data &= ~IGP01E1000_PSCFR_SMART_SPEED;
			ret_val = e1e_wphy(hw, IGP01E1000_PHY_PORT_CONFIG,
						     data);
			if (ret_val)
				return ret_val;
		}
	} else if ((phy->autoneg_advertised == E1000_ALL_SPEED_DUPLEX) ||
		   (phy->autoneg_advertised == E1000_ALL_NOT_GIG) ||
		   (phy->autoneg_advertised == E1000_ALL_10_SPEED)) {
		data |= IGP02E1000_PM_D3_LPLU;
		ret_val = e1e_wphy(hw, IGP02E1000_PHY_POWER_MGMT, data);
		if (ret_val)
			return ret_val;

		/* When LPLU is enabled, we should disable SmartSpeed */
		ret_val = e1e_rphy(hw, IGP01E1000_PHY_PORT_CONFIG, &data);
		if (ret_val)
			return ret_val;

		data &= ~IGP01E1000_PSCFR_SMART_SPEED;
		ret_val = e1e_wphy(hw, IGP01E1000_PHY_PORT_CONFIG, data);
	}

	return ret_val;
}

/**
 *  e1000e_check_downshift - Checks whether a downshift in speed occured
 *  @hw: pointer to the HW structure
 *
 *  Success returns 0, Failure returns 1
 *
 *  A downshift is detected by querying the PHY link health.
 **/
s32 e1000e_check_downshift(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 phy_data, offset, mask;

	switch (phy->type) {
	case e1000_phy_m88:
	case e1000_phy_gg82563:
		offset	= M88E1000_PHY_SPEC_STATUS;
		mask	= M88E1000_PSSR_DOWNSHIFT;
		break;
	case e1000_phy_igp_2:
	case e1000_phy_igp_3:
		offset	= IGP01E1000_PHY_LINK_HEALTH;
		mask	= IGP01E1000_PLHR_SS_DOWNGRADE;
		break;
	default:
		/* speed downshift not supported */
		phy->speed_downgraded = 0;
		return 0;
	}

	ret_val = e1e_rphy(hw, offset, &phy_data);

	if (!ret_val)
		phy->speed_downgraded = (phy_data & mask);

	return ret_val;
}

/**
 *  e1000_check_polarity_m88 - Checks the polarity.
 *  @hw: pointer to the HW structure
 *
 *  Success returns 0, Failure returns -E1000_ERR_PHY (-2)
 *
 *  Polarity is determined based on the PHY specific status register.
 **/
static s32 e1000_check_polarity_m88(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 data;

	ret_val = e1e_rphy(hw, M88E1000_PHY_SPEC_STATUS, &data);

	if (!ret_val)
		phy->cable_polarity = (data & M88E1000_PSSR_REV_POLARITY)
				      ? e1000_rev_polarity_reversed
				      : e1000_rev_polarity_normal;

	return ret_val;
}

/**
 *  e1000_check_polarity_igp - Checks the polarity.
 *  @hw: pointer to the HW structure
 *
 *  Success returns 0, Failure returns -E1000_ERR_PHY (-2)
 *
 *  Polarity is determined based on the PHY port status register, and the
 *  current speed (since there is no polarity at 100Mbps).
 **/
static s32 e1000_check_polarity_igp(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 data, offset, mask;

	/* Polarity is determined based on the speed of
	 * our connection. */
	ret_val = e1e_rphy(hw, IGP01E1000_PHY_PORT_STATUS, &data);
	if (ret_val)
		return ret_val;

	if ((data & IGP01E1000_PSSR_SPEED_MASK) ==
	    IGP01E1000_PSSR_SPEED_1000MBPS) {
		offset	= IGP01E1000_PHY_PCS_INIT_REG;
		mask	= IGP01E1000_PHY_POLARITY_MASK;
	} else {
		/* This really only applies to 10Mbps since
		 * there is no polarity for 100Mbps (always 0).
		 */
		offset	= IGP01E1000_PHY_PORT_STATUS;
		mask	= IGP01E1000_PSSR_POLARITY_REVERSED;
	}

	ret_val = e1e_rphy(hw, offset, &data);

	if (!ret_val)
		phy->cable_polarity = (data & mask)
				      ? e1000_rev_polarity_reversed
				      : e1000_rev_polarity_normal;

	return ret_val;
}

/**
 *  e1000_wait_autoneg - Wait for auto-neg compeletion
 *  @hw: pointer to the HW structure
 *
 *  Waits for auto-negotiation to complete or for the auto-negotiation time
 *  limit to expire, which ever happens first.
 **/
static s32 e1000_wait_autoneg(struct e1000_hw *hw)
{
	s32 ret_val = 0;
	u16 i, phy_status;

	/* Break after autoneg completes or PHY_AUTO_NEG_LIMIT expires. */
	for (i = PHY_AUTO_NEG_LIMIT; i > 0; i--) {
		ret_val = e1e_rphy(hw, PHY_STATUS, &phy_status);
		if (ret_val)
			break;
		ret_val = e1e_rphy(hw, PHY_STATUS, &phy_status);
		if (ret_val)
			break;
		if (phy_status & MII_SR_AUTONEG_COMPLETE)
			break;
		msleep(100);
	}

	/* PHY_AUTO_NEG_TIME expiration doesn't guarantee auto-negotiation
	 * has completed.
	 */
	return ret_val;
}

/**
 *  e1000e_phy_has_link_generic - Polls PHY for link
 *  @hw: pointer to the HW structure
 *  @iterations: number of times to poll for link
 *  @usec_interval: delay between polling attempts
 *  @success: pointer to whether polling was successful or not
 *
 *  Polls the PHY status register for link, 'iterations' number of times.
 **/
s32 e1000e_phy_has_link_generic(struct e1000_hw *hw, u32 iterations,
			       u32 usec_interval, bool *success)
{
	s32 ret_val = 0;
	u16 i, phy_status;

	for (i = 0; i < iterations; i++) {
		/* Some PHYs require the PHY_STATUS register to be read
		 * twice due to the link bit being sticky.  No harm doing
		 * it across the board.
		 */
		ret_val = e1e_rphy(hw, PHY_STATUS, &phy_status);
		if (ret_val)
			break;
		ret_val = e1e_rphy(hw, PHY_STATUS, &phy_status);
		if (ret_val)
			break;
		if (phy_status & MII_SR_LINK_STATUS)
			break;
		if (usec_interval >= 1000)
			mdelay(usec_interval/1000);
		else
			udelay(usec_interval);
	}

	*success = (i < iterations);

	return ret_val;
}

/**
 *  e1000e_get_cable_length_m88 - Determine cable length for m88 PHY
 *  @hw: pointer to the HW structure
 *
 *  Reads the PHY specific status register to retrieve the cable length
 *  information.  The cable length is determined by averaging the minimum and
 *  maximum values to get the "average" cable length.  The m88 PHY has four
 *  possible cable length values, which are:
 *	Register Value		Cable Length
 *	0			< 50 meters
 *	1			50 - 80 meters
 *	2			80 - 110 meters
 *	3			110 - 140 meters
 *	4			> 140 meters
 **/
s32 e1000e_get_cable_length_m88(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 phy_data, index;

	ret_val = e1e_rphy(hw, M88E1000_PHY_SPEC_STATUS, &phy_data);
	if (ret_val)
		return ret_val;

	index = (phy_data & M88E1000_PSSR_CABLE_LENGTH) >>
		M88E1000_PSSR_CABLE_LENGTH_SHIFT;
	phy->min_cable_length = e1000_m88_cable_length_table[index];
	phy->max_cable_length = e1000_m88_cable_length_table[index+1];

	phy->cable_length = (phy->min_cable_length + phy->max_cable_length) / 2;

	return ret_val;
}

/**
 *  e1000e_get_cable_length_igp_2 - Determine cable length for igp2 PHY
 *  @hw: pointer to the HW structure
 *
 *  The automatic gain control (agc) normalizes the amplitude of the
 *  received signal, adjusting for the attenuation produced by the
 *  cable.  By reading the AGC registers, which reperesent the
 *  cobination of course and fine gain value, the value can be put
 *  into a lookup table to obtain the approximate cable length
 *  for each channel.
 **/
s32 e1000e_get_cable_length_igp_2(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 phy_data, i, agc_value = 0;
	u16 cur_agc_index, max_agc_index = 0;
	u16 min_agc_index = IGP02E1000_CABLE_LENGTH_TABLE_SIZE - 1;
	u16 agc_reg_array[IGP02E1000_PHY_CHANNEL_NUM] =
							 {IGP02E1000_PHY_AGC_A,
							  IGP02E1000_PHY_AGC_B,
							  IGP02E1000_PHY_AGC_C,
							  IGP02E1000_PHY_AGC_D};

	/* Read the AGC registers for all channels */
	for (i = 0; i < IGP02E1000_PHY_CHANNEL_NUM; i++) {
		ret_val = e1e_rphy(hw, agc_reg_array[i], &phy_data);
		if (ret_val)
			return ret_val;

		/* Getting bits 15:9, which represent the combination of
		 * course and fine gain values.  The result is a number
		 * that can be put into the lookup table to obtain the
		 * approximate cable length. */
		cur_agc_index = (phy_data >> IGP02E1000_AGC_LENGTH_SHIFT) &
				IGP02E1000_AGC_LENGTH_MASK;

		/* Array index bound check. */
		if ((cur_agc_index >= IGP02E1000_CABLE_LENGTH_TABLE_SIZE) ||
		    (cur_agc_index == 0))
			return -E1000_ERR_PHY;

		/* Remove min & max AGC values from calculation. */
		if (e1000_igp_2_cable_length_table[min_agc_index] >
		    e1000_igp_2_cable_length_table[cur_agc_index])
			min_agc_index = cur_agc_index;
		if (e1000_igp_2_cable_length_table[max_agc_index] <
		    e1000_igp_2_cable_length_table[cur_agc_index])
			max_agc_index = cur_agc_index;

		agc_value += e1000_igp_2_cable_length_table[cur_agc_index];
	}

	agc_value -= (e1000_igp_2_cable_length_table[min_agc_index] +
		      e1000_igp_2_cable_length_table[max_agc_index]);
	agc_value /= (IGP02E1000_PHY_CHANNEL_NUM - 2);

	/* Calculate cable length with the error range of +/- 10 meters. */
	phy->min_cable_length = ((agc_value - IGP02E1000_AGC_RANGE) > 0) ?
				 (agc_value - IGP02E1000_AGC_RANGE) : 0;
	phy->max_cable_length = agc_value + IGP02E1000_AGC_RANGE;

	phy->cable_length = (phy->min_cable_length + phy->max_cable_length) / 2;

	return ret_val;
}

/**
 *  e1000e_get_phy_info_m88 - Retrieve PHY information
 *  @hw: pointer to the HW structure
 *
 *  Valid for only copper links.  Read the PHY status register (sticky read)
 *  to verify that link is up.  Read the PHY special control register to
 *  determine the polarity and 10base-T extended distance.  Read the PHY
 *  special status register to determine MDI/MDIx and current speed.  If
 *  speed is 1000, then determine cable length, local and remote receiver.
 **/
s32 e1000e_get_phy_info_m88(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32  ret_val;
	u16 phy_data;
	bool link;

	if (hw->media_type != e1000_media_type_copper) {
		hw_dbg(hw, "Phy info is only valid for copper media\n");
		return -E1000_ERR_CONFIG;
	}

	ret_val = e1000e_phy_has_link_generic(hw, 1, 0, &link);
	if (ret_val)
		return ret_val;

	if (!link) {
		hw_dbg(hw, "Phy info is only valid if link is up\n");
		return -E1000_ERR_CONFIG;
	}

	ret_val = e1e_rphy(hw, M88E1000_PHY_SPEC_CTRL, &phy_data);
	if (ret_val)
		return ret_val;

	phy->polarity_correction = (phy_data &
				    M88E1000_PSCR_POLARITY_REVERSAL);

	ret_val = e1000_check_polarity_m88(hw);
	if (ret_val)
		return ret_val;

	ret_val = e1e_rphy(hw, M88E1000_PHY_SPEC_STATUS, &phy_data);
	if (ret_val)
		return ret_val;

	phy->is_mdix = (phy_data & M88E1000_PSSR_MDIX);

	if ((phy_data & M88E1000_PSSR_SPEED) == M88E1000_PSSR_1000MBS) {
		ret_val = e1000_get_cable_length(hw);
		if (ret_val)
			return ret_val;

		ret_val = e1e_rphy(hw, PHY_1000T_STATUS, &phy_data);
		if (ret_val)
			return ret_val;

		phy->local_rx = (phy_data & SR_1000T_LOCAL_RX_STATUS)
				? e1000_1000t_rx_status_ok
				: e1000_1000t_rx_status_not_ok;

		phy->remote_rx = (phy_data & SR_1000T_REMOTE_RX_STATUS)
				 ? e1000_1000t_rx_status_ok
				 : e1000_1000t_rx_status_not_ok;
	} else {
		/* Set values to "undefined" */
		phy->cable_length = E1000_CABLE_LENGTH_UNDEFINED;
		phy->local_rx = e1000_1000t_rx_status_undefined;
		phy->remote_rx = e1000_1000t_rx_status_undefined;
	}

	return ret_val;
}

/**
 *  e1000e_get_phy_info_igp - Retrieve igp PHY information
 *  @hw: pointer to the HW structure
 *
 *  Read PHY status to determine if link is up.  If link is up, then
 *  set/determine 10base-T extended distance and polarity correction.  Read
 *  PHY port status to determine MDI/MDIx and speed.  Based on the speed,
 *  determine on the cable length, local and remote receiver.
 **/
s32 e1000e_get_phy_info_igp(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u16 data;
	bool link;

	ret_val = e1000e_phy_has_link_generic(hw, 1, 0, &link);
	if (ret_val)
		return ret_val;

	if (!link) {
		hw_dbg(hw, "Phy info is only valid if link is up\n");
		return -E1000_ERR_CONFIG;
	}

	phy->polarity_correction = 1;

	ret_val = e1000_check_polarity_igp(hw);
	if (ret_val)
		return ret_val;

	ret_val = e1e_rphy(hw, IGP01E1000_PHY_PORT_STATUS, &data);
	if (ret_val)
		return ret_val;

	phy->is_mdix = (data & IGP01E1000_PSSR_MDIX);

	if ((data & IGP01E1000_PSSR_SPEED_MASK) ==
	    IGP01E1000_PSSR_SPEED_1000MBPS) {
		ret_val = e1000_get_cable_length(hw);
		if (ret_val)
			return ret_val;

		ret_val = e1e_rphy(hw, PHY_1000T_STATUS, &data);
		if (ret_val)
			return ret_val;

		phy->local_rx = (data & SR_1000T_LOCAL_RX_STATUS)
				? e1000_1000t_rx_status_ok
				: e1000_1000t_rx_status_not_ok;

		phy->remote_rx = (data & SR_1000T_REMOTE_RX_STATUS)
				 ? e1000_1000t_rx_status_ok
				 : e1000_1000t_rx_status_not_ok;
	} else {
		phy->cable_length = E1000_CABLE_LENGTH_UNDEFINED;
		phy->local_rx = e1000_1000t_rx_status_undefined;
		phy->remote_rx = e1000_1000t_rx_status_undefined;
	}

	return ret_val;
}

/**
 *  e1000e_phy_sw_reset - PHY software reset
 *  @hw: pointer to the HW structure
 *
 *  Does a software reset of the PHY by reading the PHY control register and
 *  setting/write the control register reset bit to the PHY.
 **/
s32 e1000e_phy_sw_reset(struct e1000_hw *hw)
{
	s32 ret_val;
	u16 phy_ctrl;

	ret_val = e1e_rphy(hw, PHY_CONTROL, &phy_ctrl);
	if (ret_val)
		return ret_val;

	phy_ctrl |= MII_CR_RESET;
	ret_val = e1e_wphy(hw, PHY_CONTROL, phy_ctrl);
	if (ret_val)
		return ret_val;

	udelay(1);

	return ret_val;
}

/**
 *  e1000e_phy_hw_reset_generic - PHY hardware reset
 *  @hw: pointer to the HW structure
 *
 *  Verify the reset block is not blocking us from resetting.  Acquire
 *  semaphore (if necessary) and read/set/write the device control reset
 *  bit in the PHY.  Wait the appropriate delay time for the device to
 *  reset and relase the semaphore (if necessary).
 **/
s32 e1000e_phy_hw_reset_generic(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val;
	u32 ctrl;

	ret_val = e1000_check_reset_block(hw);
	if (ret_val)
		return 0;

	ret_val = phy->ops.acquire_phy(hw);
	if (ret_val)
		return ret_val;

	ctrl = er32(CTRL);
	ew32(CTRL, ctrl | E1000_CTRL_PHY_RST);
	e1e_flush();

	udelay(phy->reset_delay_us);

	ew32(CTRL, ctrl);
	e1e_flush();

	udelay(150);

	phy->ops.release_phy(hw);

	return e1000_get_phy_cfg_done(hw);
}

/**
 *  e1000e_get_cfg_done - Generic configuration done
 *  @hw: pointer to the HW structure
 *
 *  Generic function to wait 10 milli-seconds for configuration to complete
 *  and return success.
 **/
s32 e1000e_get_cfg_done(struct e1000_hw *hw)
{
	mdelay(10);
	return 0;
}

/* Internal function pointers */

/**
 *  e1000_get_phy_cfg_done - Generic PHY configuration done
 *  @hw: pointer to the HW structure
 *
 *  Return success if silicon family did not implement a family specific
 *  get_cfg_done function.
 **/
static s32 e1000_get_phy_cfg_done(struct e1000_hw *hw)
{
	if (hw->phy.ops.get_cfg_done)
		return hw->phy.ops.get_cfg_done(hw);

	return 0;
}

/**
 *  e1000_phy_force_speed_duplex - Generic force PHY speed/duplex
 *  @hw: pointer to the HW structure
 *
 *  When the silicon family has not implemented a forced speed/duplex
 *  function for the PHY, simply return 0.
 **/
static s32 e1000_phy_force_speed_duplex(struct e1000_hw *hw)
{
	if (hw->phy.ops.force_speed_duplex)
		return hw->phy.ops.force_speed_duplex(hw);

	return 0;
}

/**
 *  e1000e_get_phy_type_from_id - Get PHY type from id
 *  @phy_id: phy_id read from the phy
 *
 *  Returns the phy type from the id.
 **/
enum e1000_phy_type e1000e_get_phy_type_from_id(u32 phy_id)
{
	enum e1000_phy_type phy_type = e1000_phy_unknown;

	switch (phy_id) {
	case M88E1000_I_PHY_ID:
	case M88E1000_E_PHY_ID:
	case M88E1111_I_PHY_ID:
	case M88E1011_I_PHY_ID:
		phy_type = e1000_phy_m88;
		break;
	case IGP01E1000_I_PHY_ID: /* IGP 1 & 2 share this */
		phy_type = e1000_phy_igp_2;
		break;
	case GG82563_E_PHY_ID:
		phy_type = e1000_phy_gg82563;
		break;
	case IGP03E1000_E_PHY_ID:
		phy_type = e1000_phy_igp_3;
		break;
	case IFE_E_PHY_ID:
	case IFE_PLUS_E_PHY_ID:
	case IFE_C_E_PHY_ID:
		phy_type = e1000_phy_ife;
		break;
	default:
		phy_type = e1000_phy_unknown;
		break;
	}
	return phy_type;
}

/**
 *  e1000e_commit_phy - Soft PHY reset
 *  @hw: pointer to the HW structure
 *
 *  Performs a soft PHY reset on those that apply. This is a function pointer
 *  entry point called by drivers.
 **/
s32 e1000e_commit_phy(struct e1000_hw *hw)
{
	if (hw->phy.ops.commit_phy)
		return hw->phy.ops.commit_phy(hw);

	return 0;
}

/**
 *  e1000_set_d0_lplu_state - Sets low power link up state for D0
 *  @hw: pointer to the HW structure
 *  @active: boolean used to enable/disable lplu
 *
 *  Success returns 0, Failure returns 1
 *
 *  The low power link up (lplu) state is set to the power management level D0
 *  and SmartSpeed is disabled when active is true, else clear lplu for D0
 *  and enable Smartspeed.  LPLU and Smartspeed are mutually exclusive.  LPLU
 *  is used during Dx states where the power conservation is most important.
 *  During driver activity, SmartSpeed should be enabled so performance is
 *  maintained.  This is a function pointer entry point called by drivers.
 **/
static s32 e1000_set_d0_lplu_state(struct e1000_hw *hw, bool active)
{
	if (hw->phy.ops.set_d0_lplu_state)
		return hw->phy.ops.set_d0_lplu_state(hw, active);

	return 0;
}
