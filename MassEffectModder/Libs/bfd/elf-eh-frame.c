/* .eh_frame section optimization.
   Copyright (C) 2001-2019 Free Software Foundation, Inc.
   Written by Jakub Jelinek <jakub@redhat.com>.

   This file is part of BFD, the Binary File Descriptor library.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston,
   MA 02110-1301, USA.  */

#include "sysdep.h"
#include "bfd.h"
#include "libbfd.h"
#include "elf-bfd.h"
#include "dwarf2.h"

#define EH_FRAME_HDR_SIZE 8

struct cie
{
  unsigned int length;
  unsigned int hash;
  unsigned char version;
  unsigned char local_personality;
  char augmentation[20];
  bfd_vma code_align;
  bfd_signed_vma data_align;
  bfd_vma ra_column;
  bfd_vma augmentation_size;
  union {
    struct elf_link_hash_entry *h;
    struct {
      unsigned int bfd_id;
      unsigned int index;
    } sym;
    unsigned int reloc_index;
  } personality;
  struct eh_cie_fde *cie_inf;
  unsigned char per_encoding;
  unsigned char lsda_encoding;
  unsigned char fde_encoding;
  unsigned char initial_insn_length;
  unsigned char can_make_lsda_relative;
  unsigned char initial_instructions[50];
};



/* If *ITER hasn't reached END yet, read the next byte into *RESULT and
   move onto the next byte.  Return true on success.  */

static inline bfd_boolean
read_byte (bfd_byte **iter, bfd_byte *end, unsigned char *result)
{
  if (*iter >= end)
    return FALSE;
  *result = *((*iter)++);
  return TRUE;
}

/* Move *ITER over an leb128, stopping at END.  Return true if the end
   of the leb128 was found.  */

static bfd_boolean
skip_leb128 (bfd_byte **iter, bfd_byte *end)
{
  unsigned char byte;
  do
    if (!read_byte (iter, end, &byte))
      return FALSE;
  while (byte & 0x80);
  return TRUE;
}

/* Return 0 if either encoding is variable width, or not yet known to bfd.  */

static
int get_DW_EH_PE_width (int encoding, int ptr_size)
{
  /* DW_EH_PE_ values of 0x60 and 0x70 weren't defined at the time .eh_frame
     was added to bfd.  */
  if ((encoding & 0x60) == 0x60)
    return 0;

  switch (encoding & 7)
    {
    case DW_EH_PE_udata2: return 2;
    case DW_EH_PE_udata4: return 4;
    case DW_EH_PE_udata8: return 8;
    case DW_EH_PE_absptr: return ptr_size;
    default:
      break;
    }

  return 0;
}

#define get_DW_EH_PE_signed(encoding) (((encoding) & DW_EH_PE_signed) != 0)

/* Read a width sized value from memory.  */

static bfd_vma
read_value (bfd *abfd, bfd_byte *buf, int width, int is_signed)
{
  bfd_vma value;

  switch (width)
    {
    case 2:
      if (is_signed)
	value = bfd_get_signed_16 (abfd, buf);
      else
	value = bfd_get_16 (abfd, buf);
      break;
    case 4:
      if (is_signed)
	value = bfd_get_signed_32 (abfd, buf);
      else
	value = bfd_get_32 (abfd, buf);
      break;
    case 8:
      if (is_signed)
	value = bfd_get_signed_64 (abfd, buf);
      else
	value = bfd_get_64 (abfd, buf);
      break;
    default:
      BFD_FAIL ();
      return 0;
    }

  return value;
}

/* Store a width sized value to memory.  */

static void
write_value (bfd *abfd, bfd_byte *buf, bfd_vma value, int width)
{
  switch (width)
    {
    case 2: bfd_put_16 (abfd, value, buf); break;
    case 4: bfd_put_32 (abfd, value, buf); break;
    case 8: bfd_put_64 (abfd, value, buf); break;
    default: BFD_FAIL ();
    }
}

/* Return the number of extra bytes that we'll be inserting into
   ENTRY's augmentation string.  */

static INLINE unsigned int
extra_augmentation_string_bytes (struct eh_cie_fde *entry)
{
  unsigned int size = 0;
  if (entry->cie)
    {
      if (entry->add_augmentation_size)
	size++;
      if (entry->u.cie.add_fde_encoding)
	size++;
    }
  return size;
}

/* Likewise ENTRY's augmentation data.  */

static INLINE unsigned int
extra_augmentation_data_bytes (struct eh_cie_fde *entry)
{
  unsigned int size = 0;
  if (entry->add_augmentation_size)
    size++;
  if (entry->cie && entry->u.cie.add_fde_encoding)
    size++;
  return size;
}

/* Return the offset of the FDE or CIE after ENT.  */

static unsigned int
next_cie_fde_offset (const struct eh_cie_fde *ent,
		     const struct eh_cie_fde *last,
		     const asection *sec)
{
  while (++ent < last)
    {
      if (!ent->removed)
	return ent->new_offset;
    }
  return sec->size;
}

/* Convert absolute encoding ENCODING into PC-relative form.
   SIZE is the size of a pointer.  */

static unsigned char
make_pc_relative (unsigned char encoding, unsigned int ptr_size)
{
  if ((encoding & 0x7f) == DW_EH_PE_absptr)
    switch (ptr_size)
      {
      case 2:
	encoding |= DW_EH_PE_sdata2;
	break;
      case 4:
	encoding |= DW_EH_PE_sdata4;
	break;
      case 8:
	encoding |= DW_EH_PE_sdata8;
	break;
      }
  return encoding | DW_EH_PE_pcrel;
}

/*  Examine each .eh_frame_entry section and discard those
    those that are marked SEC_EXCLUDE.  */

static void
bfd_elf_discard_eh_frame_entry (struct eh_frame_hdr_info *hdr_info)
{
  unsigned int i;
  for (i = 0; i < hdr_info->array_count; i++)
    {
      if (hdr_info->u.compact.entries[i]->flags & SEC_EXCLUDE)
	{
	  unsigned int j;
	  for (j = i + 1; j < hdr_info->array_count; j++)
	    hdr_info->u.compact.entries[j-1] = hdr_info->u.compact.entries[j];

	  hdr_info->array_count--;
	  hdr_info->u.compact.entries[hdr_info->array_count] = NULL;
	  i--;
	}
    }
}

/* Parse a .eh_frame_entry section.  Figure out which text section it
   references.  */

bfd_boolean
_bfd_elf_parse_eh_frame_entry (struct bfd_link_info *info ATTRIBUTE_UNUSED,
                   asection *sec ATTRIBUTE_UNUSED, struct elf_reloc_cookie *cookie ATTRIBUTE_UNUSED)
{
  return TRUE;
}

/* Try to parse .eh_frame section SEC, which belongs to ABFD.  Store the
   information in the section's sec_info field on success.  COOKIE
   describes the relocations in SEC.  */

void
_bfd_elf_parse_eh_frame (bfd *abfd ATTRIBUTE_UNUSED, struct bfd_link_info *info ATTRIBUTE_UNUSED,
             asection *sec ATTRIBUTE_UNUSED, struct elf_reloc_cookie *cookie ATTRIBUTE_UNUSED)
{
}

/* Order eh_frame_hdr entries by the VMA of their text section.  */

static int
cmp_eh_frame_hdr (const void *a, const void *b)
{
  bfd_vma text_a;
  bfd_vma text_b;
  asection *sec;

  sec = *(asection *const *)a;
  sec = (asection *) elf_section_data (sec)->sec_info;
  text_a = sec->output_section->vma + sec->output_offset;
  sec = *(asection *const *)b;
  sec = (asection *) elf_section_data (sec)->sec_info;
  text_b = sec->output_section->vma + sec->output_offset;

  if (text_a < text_b)
    return -1;
  return text_a > text_b;

}

/* Add space for a CANTUNWIND terminator to SEC if the text sections
   referenced by it and NEXT are not contiguous, or NEXT is NULL.  */

static void
add_eh_frame_hdr_terminator (asection *sec,
			     asection *next)
{
  bfd_vma end;
  bfd_vma next_start;
  asection *text_sec;

  if (next)
    {
      /* See if there is a gap (presumably a text section without unwind info)
	 between these two entries.  */
      text_sec = (asection *) elf_section_data (sec)->sec_info;
      end = text_sec->output_section->vma + text_sec->output_offset
	    + text_sec->size;
      text_sec = (asection *) elf_section_data (next)->sec_info;
      next_start = text_sec->output_section->vma + text_sec->output_offset;
      if (end == next_start)
	return;
    }

  /* Add space for a CANTUNWIND terminator.  */
  if (!sec->rawsize)
    sec->rawsize = sec->size;

  bfd_set_section_size (sec->owner, sec, sec->size + 8);
}

/* Finish a pass over all .eh_frame_entry sections.  */

bfd_boolean
_bfd_elf_end_eh_frame_parsing (struct bfd_link_info *info)
{
  struct eh_frame_hdr_info *hdr_info;
  unsigned int i;

  hdr_info = &elf_hash_table (info)->eh_info;

  if (info->eh_frame_hdr_type != COMPACT_EH_HDR
      || hdr_info->array_count == 0)
    return FALSE;

  bfd_elf_discard_eh_frame_entry (hdr_info);

  qsort (hdr_info->u.compact.entries, hdr_info->array_count,
	 sizeof (asection *), cmp_eh_frame_hdr);

  for (i = 0; i < hdr_info->array_count - 1; i++)
    {
      add_eh_frame_hdr_terminator (hdr_info->u.compact.entries[i],
				   hdr_info->u.compact.entries[i + 1]);
    }

  /* Add a CANTUNWIND terminator after the last entry.  */
  add_eh_frame_hdr_terminator (hdr_info->u.compact.entries[i], NULL);
  return TRUE;
}

/* Mark all relocations against CIE or FDE ENT, which occurs in
   .eh_frame section SEC.  COOKIE describes the relocations in SEC;
   its "rel" field can be changed freely.  */

static bfd_boolean
mark_entry (struct bfd_link_info *info ATTRIBUTE_UNUSED, asection *sec ATTRIBUTE_UNUSED,
        struct eh_cie_fde *ent ATTRIBUTE_UNUSED, elf_gc_mark_hook_fn gc_mark_hook ATTRIBUTE_UNUSED,
        struct elf_reloc_cookie *cookie ATTRIBUTE_UNUSED)
{
  return TRUE;
}

/* Mark all the relocations against FDEs that relate to code in input
   section SEC.  The FDEs belong to .eh_frame section EH_FRAME, whose
   relocations are described by COOKIE.  */

bfd_boolean
_bfd_elf_gc_mark_fdes (struct bfd_link_info *info, asection *sec,
		       asection *eh_frame, elf_gc_mark_hook_fn gc_mark_hook,
		       struct elf_reloc_cookie *cookie)
{
  struct eh_cie_fde *fde, *cie;

  for (fde = elf_fde_list (sec); fde; fde = fde->u.fde.next_for_section)
    {
      if (!mark_entry (info, eh_frame, fde, gc_mark_hook, cookie))
	return FALSE;

      /* At this stage, all cie_inf fields point to local CIEs, so we
	 can use the same cookie to refer to them.  */
      cie = fde->u.fde.cie_inf;
      if (cie != NULL && !cie->u.cie.gc_mark)
	{
	  cie->u.cie.gc_mark = 1;
	  if (!mark_entry (info, eh_frame, cie, gc_mark_hook, cookie))
	    return FALSE;
	}
    }
  return TRUE;
}

/* For a given OFFSET in SEC, return the delta to the new location
   after .eh_frame editing.  */

static bfd_signed_vma
offset_adjust (bfd_vma offset, const asection *sec)
{
  struct eh_frame_sec_info *sec_info
    = (struct eh_frame_sec_info *) elf_section_data (sec)->sec_info;
  unsigned int lo, hi, mid;
  struct eh_cie_fde *ent = NULL;
  bfd_signed_vma delta;

  lo = 0;
  hi = sec_info->count;
  if (hi == 0)
    return 0;

  while (lo < hi)
    {
      mid = (lo + hi) / 2;
      ent = &sec_info->entry[mid];
      if (offset < ent->offset)
	hi = mid;
      else if (mid + 1 >= hi)
	break;
      else if (offset >= ent[1].offset)
	lo = mid + 1;
      else
	break;
    }

  if (!ent->removed)
    delta = (bfd_vma) ent->new_offset - (bfd_vma) ent->offset;
  else if (ent->cie && ent->u.cie.merged)
    {
      struct eh_cie_fde *cie = ent->u.cie.u.merged_with;
      delta = ((bfd_vma) cie->new_offset + cie->u.cie.u.sec->output_offset
	       - (bfd_vma) ent->offset - sec->output_offset);
    }
  else
    {
      /* Is putting the symbol on the next entry best for a deleted
	 CIE/FDE?  */
      struct eh_cie_fde *last = sec_info->entry + sec_info->count;
      delta = ((bfd_vma) next_cie_fde_offset (ent, last, sec)
	       - (bfd_vma) ent->offset);
      return delta;
    }

  /* Account for editing within this CIE/FDE.  */
  offset -= ent->offset;
  if (ent->cie)
    {
      unsigned int extra
	= ent->add_augmentation_size + ent->u.cie.add_fde_encoding;
      if (extra == 0
	  || offset <= 9u + ent->u.cie.aug_str_len)
	return delta;
      delta += extra;
      if (offset <= 9u + ent->u.cie.aug_str_len + ent->u.cie.aug_data_len)
	return delta;
      delta += extra;
    }
  else
    {
      unsigned int ptr_size, width, extra = ent->add_augmentation_size;
      if (offset <= 12 || extra == 0)
	return delta;
      ptr_size = (get_elf_backend_data (sec->owner)
		  ->elf_backend_eh_frame_address_size (sec->owner, sec));
      width = get_DW_EH_PE_width (ent->fde_encoding, ptr_size);
      if (offset <= 8 + 2 * width)
	return delta;
      delta += extra;
    }

  return delta;
}

/* Adjust a global symbol defined in .eh_frame, so that it stays
   relative to its original CIE/FDE.  It is assumed that a symbol
   defined at the beginning of a CIE/FDE belongs to that CIE/FDE
   rather than marking the end of the previous CIE/FDE.  This matters
   when a CIE is merged with a previous CIE, since the symbol is
   moved to the merged CIE.  */

bfd_boolean
_bfd_elf_adjust_eh_frame_global_symbol (struct elf_link_hash_entry *h,
					void *arg ATTRIBUTE_UNUSED)
{
  asection *sym_sec;
  bfd_signed_vma delta;

  if (h->root.type != bfd_link_hash_defined
      && h->root.type != bfd_link_hash_defweak)
    return TRUE;

  sym_sec = h->root.u.def.section;
  if (sym_sec->sec_info_type != SEC_INFO_TYPE_EH_FRAME
      || elf_section_data (sym_sec)->sec_info == NULL)
    return TRUE;

  delta = offset_adjust (h->root.u.def.value, sym_sec);
  h->root.u.def.value += delta;

  return TRUE;
}

/* This function is called for each input file before the .eh_frame
   section is relocated.  It discards duplicate CIEs and FDEs for discarded
   functions.  The function returns TRUE iff any entries have been
   deleted.  */

bfd_boolean
_bfd_elf_discard_section_eh_frame
   (bfd *abfd ATTRIBUTE_UNUSED, struct bfd_link_info *info ATTRIBUTE_UNUSED, asection *sec ATTRIBUTE_UNUSED,
    bfd_boolean (*reloc_symbol_deleted_p) (bfd_vma, void *) ATTRIBUTE_UNUSED,
    struct elf_reloc_cookie *cookie ATTRIBUTE_UNUSED)
{
  return FALSE;
}

/* This function is called for .eh_frame_hdr section after
   _bfd_elf_discard_section_eh_frame has been called on all .eh_frame
   input sections.  It finalizes the size of .eh_frame_hdr section.  */

bfd_boolean
_bfd_elf_discard_section_eh_frame_hdr (bfd *abfd, struct bfd_link_info *info)
{
  struct elf_link_hash_table *htab;
  struct eh_frame_hdr_info *hdr_info;
  asection *sec;

  htab = elf_hash_table (info);
  hdr_info = &htab->eh_info;

  if (!hdr_info->frame_hdr_is_compact && hdr_info->u.dwarf.cies != NULL)
    {
      htab_delete (hdr_info->u.dwarf.cies);
      hdr_info->u.dwarf.cies = NULL;
    }

  sec = hdr_info->hdr_sec;
  if (sec == NULL)
    return FALSE;

  if (info->eh_frame_hdr_type == COMPACT_EH_HDR)
    {
      /* For compact frames we only add the header.  The actual table comes
	 from the .eh_frame_entry sections.  */
      sec->size = 8;
    }
  else
    {
      sec->size = EH_FRAME_HDR_SIZE;
      if (hdr_info->u.dwarf.table)
	sec->size += 4 + hdr_info->u.dwarf.fde_count * 8;
    }

  elf_eh_frame_hdr (abfd) = sec;
  return TRUE;
}

/* Return true if there is at least one non-empty .eh_frame section in
   input files.  Can only be called after ld has mapped input to
   output sections, and before sections are stripped.  */

bfd_boolean
_bfd_elf_eh_frame_present (struct bfd_link_info *info)
{
  asection *eh = bfd_get_section_by_name (info->output_bfd, ".eh_frame");

  if (eh == NULL)
    return FALSE;

  /* Count only sections which have at least a single CIE or FDE.
     There cannot be any CIE or FDE <= 8 bytes.  */
  for (eh = eh->map_head.s; eh != NULL; eh = eh->map_head.s)
    if (eh->size > 8)
      return TRUE;

  return FALSE;
}

/* Return true if there is at least one .eh_frame_entry section in
   input files.  */

bfd_boolean
_bfd_elf_eh_frame_entry_present (struct bfd_link_info *info)
{
  asection *o;
  bfd *abfd;

  for (abfd = info->input_bfds; abfd != NULL; abfd = abfd->link.next)
    {
      for (o = abfd->sections; o; o = o->next)
	{
	  const char *name = bfd_get_section_name (abfd, o);

	  if (strcmp (name, ".eh_frame_entry")
	      && !bfd_is_abs_section (o->output_section))
	    return TRUE;
	}
    }
  return FALSE;
}

/* This function is called from size_dynamic_sections.
   It needs to decide whether .eh_frame_hdr should be output or not,
   because when the dynamic symbol table has been sized it is too late
   to strip sections.  */

bfd_boolean
_bfd_elf_maybe_strip_eh_frame_hdr (struct bfd_link_info *info)
{
  struct elf_link_hash_table *htab;
  struct eh_frame_hdr_info *hdr_info;
  struct bfd_link_hash_entry *bh = NULL;
  struct elf_link_hash_entry *h;

  htab = elf_hash_table (info);
  hdr_info = &htab->eh_info;
  if (hdr_info->hdr_sec == NULL)
    return TRUE;

  if (bfd_is_abs_section (hdr_info->hdr_sec->output_section)
      || info->eh_frame_hdr_type == 0
      || (info->eh_frame_hdr_type == DWARF2_EH_HDR
	  && !_bfd_elf_eh_frame_present (info))
      || (info->eh_frame_hdr_type == COMPACT_EH_HDR
	  && !_bfd_elf_eh_frame_entry_present (info)))
    {
      hdr_info->hdr_sec->flags |= SEC_EXCLUDE;
      hdr_info->hdr_sec = NULL;
      return TRUE;
    }

  /* Add a hidden symbol so that systems without access to PHDRs can
     find the table.  */
  if (! (_bfd_generic_link_add_one_symbol
	 (info, info->output_bfd, "__GNU_EH_FRAME_HDR", BSF_LOCAL,
	  hdr_info->hdr_sec, 0, NULL, FALSE, FALSE, &bh)))
    return FALSE;

  h = (struct elf_link_hash_entry *) bh;
  h->def_regular = 1;
  h->other = STV_HIDDEN;
  get_elf_backend_data
    (info->output_bfd)->elf_backend_hide_symbol (info, h, TRUE);

  if (!hdr_info->frame_hdr_is_compact)
    hdr_info->u.dwarf.table = TRUE;
  return TRUE;
}

/* Adjust an address in the .eh_frame section.  Given OFFSET within
   SEC, this returns the new offset in the adjusted .eh_frame section,
   or -1 if the address refers to a CIE/FDE which has been removed
   or to offset with dynamic relocation which is no longer needed.  */

bfd_vma
_bfd_elf_eh_frame_section_offset (bfd *output_bfd ATTRIBUTE_UNUSED,
				  struct bfd_link_info *info ATTRIBUTE_UNUSED,
				  asection *sec,
				  bfd_vma offset)
{
  struct eh_frame_sec_info *sec_info;
  unsigned int lo, hi, mid;

  if (sec->sec_info_type != SEC_INFO_TYPE_EH_FRAME)
    return offset;
  sec_info = (struct eh_frame_sec_info *) elf_section_data (sec)->sec_info;

  if (offset >= sec->rawsize)
    return offset - sec->rawsize + sec->size;

  lo = 0;
  hi = sec_info->count;
  mid = 0;
  while (lo < hi)
    {
      mid = (lo + hi) / 2;
      if (offset < sec_info->entry[mid].offset)
	hi = mid;
      else if (offset
	       >= sec_info->entry[mid].offset + sec_info->entry[mid].size)
	lo = mid + 1;
      else
	break;
    }

  BFD_ASSERT (lo < hi);

  /* FDE or CIE was removed.  */
  if (sec_info->entry[mid].removed)
    return (bfd_vma) -1;

  /* If converting personality pointers to DW_EH_PE_pcrel, there will be
     no need for run-time relocation against the personality field.  */
  if (sec_info->entry[mid].cie
      && sec_info->entry[mid].u.cie.make_per_encoding_relative
      && offset == (sec_info->entry[mid].offset + 8
		    + sec_info->entry[mid].u.cie.personality_offset))
    return (bfd_vma) -2;

  /* If converting to DW_EH_PE_pcrel, there will be no need for run-time
     relocation against FDE's initial_location field.  */
  if (!sec_info->entry[mid].cie
      && sec_info->entry[mid].make_relative
      && offset == sec_info->entry[mid].offset + 8)
    return (bfd_vma) -2;

  /* If converting LSDA pointers to DW_EH_PE_pcrel, there will be no need
     for run-time relocation against LSDA field.  */
  if (!sec_info->entry[mid].cie
      && sec_info->entry[mid].u.fde.cie_inf->u.cie.make_lsda_relative
      && offset == (sec_info->entry[mid].offset + 8
		    + sec_info->entry[mid].lsda_offset))
    return (bfd_vma) -2;

  /* If converting to DW_EH_PE_pcrel, there will be no need for run-time
     relocation against DW_CFA_set_loc's arguments.  */
  if (sec_info->entry[mid].set_loc
      && sec_info->entry[mid].make_relative
      && (offset >= sec_info->entry[mid].offset + 8
		    + sec_info->entry[mid].set_loc[1]))
    {
      unsigned int cnt;

      for (cnt = 1; cnt <= sec_info->entry[mid].set_loc[0]; cnt++)
	if (offset == sec_info->entry[mid].offset + 8
		      + sec_info->entry[mid].set_loc[cnt])
	  return (bfd_vma) -2;
    }

  /* Any new augmentation bytes go before the first relocation.  */
  return (offset + sec_info->entry[mid].new_offset
	  - sec_info->entry[mid].offset
	  + extra_augmentation_string_bytes (sec_info->entry + mid)
	  + extra_augmentation_data_bytes (sec_info->entry + mid));
}

/* Write out .eh_frame_entry section.  Add CANTUNWIND terminator if needed.
   Also check that the contents look sane.  */

bfd_boolean
_bfd_elf_write_section_eh_frame_entry (bfd *abfd, struct bfd_link_info *info,
				       asection *sec, bfd_byte *contents)
{
  const struct elf_backend_data *bed;
  bfd_byte cantunwind[8];
  bfd_vma addr;
  bfd_vma last_addr;
  bfd_vma offset;
  asection *text_sec = (asection *) elf_section_data (sec)->sec_info;

  if (!sec->rawsize)
    sec->rawsize = sec->size;

  BFD_ASSERT (sec->sec_info_type == SEC_INFO_TYPE_EH_FRAME_ENTRY);

  /* Check to make sure that the text section corresponding to this eh_frame_entry
     section has not been excluded.  In particular, mips16 stub entries will be
     excluded outside of the normal process.  */
  if (sec->flags & SEC_EXCLUDE
      || text_sec->flags & SEC_EXCLUDE)
    return TRUE;

  if (!bfd_set_section_contents (abfd, sec->output_section, contents,
				 sec->output_offset, sec->rawsize))
      return FALSE;

  last_addr = bfd_get_signed_32 (abfd, contents);
  /* Check that all the entries are in order.  */
  for (offset = 8; offset < sec->rawsize; offset += 8)
    {
      addr = bfd_get_signed_32 (abfd, contents + offset) + offset;
      if (addr <= last_addr)
	{
	  /* xgettext:c-format */
	  _bfd_error_handler (_("%pB: %pA not in order"), sec->owner, sec);
	  return FALSE;
	}

      last_addr = addr;
    }

  addr = text_sec->output_section->vma + text_sec->output_offset
	 + text_sec->size;
  addr &= ~1;
  addr -= (sec->output_section->vma + sec->output_offset + sec->rawsize);
  if (addr & 1)
    {
      /* xgettext:c-format */
      _bfd_error_handler (_("%pB: %pA invalid input section size"),
			  sec->owner, sec);
      bfd_set_error (bfd_error_bad_value);
      return FALSE;
    }
  if (last_addr >= addr + sec->rawsize)
    {
      /* xgettext:c-format */
      _bfd_error_handler (_("%pB: %pA points past end of text section"),
			  sec->owner, sec);
      bfd_set_error (bfd_error_bad_value);
      return FALSE;
    }

  if (sec->size == sec->rawsize)
    return TRUE;

  bed = get_elf_backend_data (abfd);
  BFD_ASSERT (sec->size == sec->rawsize + 8);
  BFD_ASSERT ((addr & 1) == 0);
  BFD_ASSERT (bed->cant_unwind_opcode);

  bfd_put_32 (abfd, addr, cantunwind);
  bfd_put_32 (abfd, (*bed->cant_unwind_opcode) (info), cantunwind + 4);
  return bfd_set_section_contents (abfd, sec->output_section, cantunwind,
				   sec->output_offset + sec->rawsize, 8);
}

/* Write out .eh_frame section.  This is called with the relocated
   contents.  */

bfd_boolean
_bfd_elf_write_section_eh_frame (bfd *abfd,
				 struct bfd_link_info *info,
				 asection *sec,
				 bfd_byte *contents)
{
  struct eh_frame_sec_info *sec_info;
  struct elf_link_hash_table *htab;
  struct eh_frame_hdr_info *hdr_info;
  unsigned int ptr_size;
  struct eh_cie_fde *ent, *last_ent;

  if (sec->sec_info_type != SEC_INFO_TYPE_EH_FRAME)
    /* FIXME: octets_per_byte.  */
    return bfd_set_section_contents (abfd, sec->output_section, contents,
				     sec->output_offset, sec->size);

  ptr_size = (get_elf_backend_data (abfd)
	      ->elf_backend_eh_frame_address_size (abfd, sec));
  BFD_ASSERT (ptr_size != 0);

  sec_info = (struct eh_frame_sec_info *) elf_section_data (sec)->sec_info;
  htab = elf_hash_table (info);
  hdr_info = &htab->eh_info;

  if (hdr_info->u.dwarf.table && hdr_info->u.dwarf.array == NULL)
    {
      hdr_info->frame_hdr_is_compact = FALSE;
      hdr_info->u.dwarf.array = (struct eh_frame_array_ent *)
	bfd_malloc (hdr_info->u.dwarf.fde_count
		    * sizeof (*hdr_info->u.dwarf.array));
    }
  if (hdr_info->u.dwarf.array == NULL)
    hdr_info = NULL;

  /* The new offsets can be bigger or smaller than the original offsets.
     We therefore need to make two passes over the section: one backward
     pass to move entries up and one forward pass to move entries down.
     The two passes won't interfere with each other because entries are
     not reordered  */
  for (ent = sec_info->entry + sec_info->count; ent-- != sec_info->entry;)
    if (!ent->removed && ent->new_offset > ent->offset)
      memmove (contents + ent->new_offset, contents + ent->offset, ent->size);

  for (ent = sec_info->entry; ent < sec_info->entry + sec_info->count; ++ent)
    if (!ent->removed && ent->new_offset < ent->offset)
      memmove (contents + ent->new_offset, contents + ent->offset, ent->size);

  last_ent = sec_info->entry + sec_info->count;
  for (ent = sec_info->entry; ent < last_ent; ++ent)
    {
      unsigned char *buf, *end;
      unsigned int new_size;

      if (ent->removed)
	continue;

      if (ent->size == 4)
	{
	  /* Any terminating FDE must be at the end of the section.  */
	  BFD_ASSERT (ent == last_ent - 1);
	  continue;
	}

      buf = contents + ent->new_offset;
      end = buf + ent->size;
      new_size = next_cie_fde_offset (ent, last_ent, sec) - ent->new_offset;

      /* Update the size.  It may be shrinked.  */
      bfd_put_32 (abfd, new_size - 4, buf);

      /* Filling the extra bytes with DW_CFA_nops.  */
      if (new_size != ent->size)
	memset (end, 0, new_size - ent->size);

      if (ent->cie)
	{
	  /* CIE */
	  if (ent->make_relative
	      || ent->u.cie.make_lsda_relative
	      || ent->u.cie.per_encoding_relative)
	    {
	      char *aug;
	      unsigned int action, extra_string, extra_data;
	      unsigned int per_width, per_encoding;

	      /* Need to find 'R' or 'L' augmentation's argument and modify
		 DW_EH_PE_* value.  */
	      action = ((ent->make_relative ? 1 : 0)
			| (ent->u.cie.make_lsda_relative ? 2 : 0)
			| (ent->u.cie.per_encoding_relative ? 4 : 0));
	      extra_string = extra_augmentation_string_bytes (ent);
	      extra_data = extra_augmentation_data_bytes (ent);

	      /* Skip length, id and version.  */
	      buf += 9;
	      aug = (char *) buf;
	      buf += strlen (aug) + 1;
	      skip_leb128 (&buf, end);
	      skip_leb128 (&buf, end);
	      skip_leb128 (&buf, end);
	      if (*aug == 'z')
		{
		  /* The uleb128 will always be a single byte for the kind
		     of augmentation strings that we're prepared to handle.  */
		  *buf++ += extra_data;
		  aug++;
		}

	      /* Make room for the new augmentation string and data bytes.  */
	      memmove (buf + extra_string + extra_data, buf, end - buf);
	      memmove (aug + extra_string, aug, buf - (bfd_byte *) aug);
	      buf += extra_string;
	      end += extra_string + extra_data;

	      if (ent->add_augmentation_size)
		{
		  *aug++ = 'z';
		  *buf++ = extra_data - 1;
		}
	      if (ent->u.cie.add_fde_encoding)
		{
		  BFD_ASSERT (action & 1);
		  *aug++ = 'R';
		  *buf++ = make_pc_relative (DW_EH_PE_absptr, ptr_size);
		  action &= ~1;
		}

	      while (action)
		switch (*aug++)
		  {
		  case 'L':
		    if (action & 2)
		      {
			BFD_ASSERT (*buf == ent->lsda_encoding);
			*buf = make_pc_relative (*buf, ptr_size);
			action &= ~2;
		      }
		    buf++;
		    break;
		  case 'P':
		    if (ent->u.cie.make_per_encoding_relative)
		      *buf = make_pc_relative (*buf, ptr_size);
		    per_encoding = *buf++;
		    per_width = get_DW_EH_PE_width (per_encoding, ptr_size);
		    BFD_ASSERT (per_width != 0);
		    BFD_ASSERT (((per_encoding & 0x70) == DW_EH_PE_pcrel)
				== ent->u.cie.per_encoding_relative);
		    if ((per_encoding & 0x70) == DW_EH_PE_aligned)
		      buf = (contents
			     + ((buf - contents + per_width - 1)
				& ~((bfd_size_type) per_width - 1)));
		    if (action & 4)
		      {
			bfd_vma val;

			val = read_value (abfd, buf, per_width,
					  get_DW_EH_PE_signed (per_encoding));
			if (ent->u.cie.make_per_encoding_relative)
			  val -= (sec->output_section->vma
				  + sec->output_offset
				  + (buf - contents));
			else
			  {
			    val += (bfd_vma) ent->offset - ent->new_offset;
			    val -= extra_string + extra_data;
			  }
			write_value (abfd, buf, val, per_width);
			action &= ~4;
		      }
		    buf += per_width;
		    break;
		  case 'R':
		    if (action & 1)
		      {
			BFD_ASSERT (*buf == ent->fde_encoding);
			*buf = make_pc_relative (*buf, ptr_size);
			action &= ~1;
		      }
		    buf++;
		    break;
		  case 'S':
		    break;
		  default:
		    BFD_FAIL ();
		  }
	    }
	}
      else
	{
	  /* FDE */
	  bfd_vma value, address;
	  unsigned int width;
	  bfd_byte *start;
	  struct eh_cie_fde *cie;

	  /* Skip length.  */
	  cie = ent->u.fde.cie_inf;
	  buf += 4;
	  value = ((ent->new_offset + sec->output_offset + 4)
		   - (cie->new_offset + cie->u.cie.u.sec->output_offset));
	  bfd_put_32 (abfd, value, buf);
	  if (bfd_link_relocatable (info))
	    continue;
	  buf += 4;
	  width = get_DW_EH_PE_width (ent->fde_encoding, ptr_size);
	  value = read_value (abfd, buf, width,
			      get_DW_EH_PE_signed (ent->fde_encoding));
	  address = value;
	  if (value)
	    {
	      switch (ent->fde_encoding & 0x70)
		{
		case DW_EH_PE_textrel:
		  BFD_ASSERT (hdr_info == NULL);
		  break;
		case DW_EH_PE_datarel:
		  {
		    switch (abfd->arch_info->arch)
		      {
		      case bfd_arch_ia64:
			BFD_ASSERT (elf_gp (abfd) != 0);
			address += elf_gp (abfd);
			break;
		      default:
			_bfd_error_handler
			  (_("DW_EH_PE_datarel unspecified"
			     " for this architecture"));
			/* Fall thru */
		      case bfd_arch_frv:
		      case bfd_arch_i386:
			BFD_ASSERT (htab->hgot != NULL
				    && ((htab->hgot->root.type
					 == bfd_link_hash_defined)
					|| (htab->hgot->root.type
					    == bfd_link_hash_defweak)));
			address
			  += (htab->hgot->root.u.def.value
			      + htab->hgot->root.u.def.section->output_offset
			      + (htab->hgot->root.u.def.section->output_section
				 ->vma));
			break;
		      }
		  }
		  break;
		case DW_EH_PE_pcrel:
		  value += (bfd_vma) ent->offset - ent->new_offset;
		  address += (sec->output_section->vma
			      + sec->output_offset
			      + ent->offset + 8);
		  break;
		}
	      if (ent->make_relative)
		value -= (sec->output_section->vma
			  + sec->output_offset
			  + ent->new_offset + 8);
	      write_value (abfd, buf, value, width);
	    }

	  start = buf;

	  if (hdr_info)
	    {
	      /* The address calculation may overflow, giving us a
		 value greater than 4G on a 32-bit target when
		 dwarf_vma is 64-bit.  */
	      if (sizeof (address) > 4 && ptr_size == 4)
		address &= 0xffffffff;
	      hdr_info->u.dwarf.array[hdr_info->array_count].initial_loc
		= address;
	      hdr_info->u.dwarf.array[hdr_info->array_count].range
		= read_value (abfd, buf + width, width, FALSE);
	      hdr_info->u.dwarf.array[hdr_info->array_count++].fde
		= (sec->output_section->vma
		   + sec->output_offset
		   + ent->new_offset);
	    }

	  if ((ent->lsda_encoding & 0x70) == DW_EH_PE_pcrel
	      || cie->u.cie.make_lsda_relative)
	    {
	      buf += ent->lsda_offset;
	      width = get_DW_EH_PE_width (ent->lsda_encoding, ptr_size);
	      value = read_value (abfd, buf, width,
				  get_DW_EH_PE_signed (ent->lsda_encoding));
	      if (value)
		{
		  if ((ent->lsda_encoding & 0x70) == DW_EH_PE_pcrel)
		    value += (bfd_vma) ent->offset - ent->new_offset;
		  else if (cie->u.cie.make_lsda_relative)
		    value -= (sec->output_section->vma
			      + sec->output_offset
			      + ent->new_offset + 8 + ent->lsda_offset);
		  write_value (abfd, buf, value, width);
		}
	    }
	  else if (ent->add_augmentation_size)
	    {
	      /* Skip the PC and length and insert a zero byte for the
		 augmentation size.  */
	      buf += width * 2;
	      memmove (buf + 1, buf, end - buf);
	      *buf = 0;
	    }

	  if (ent->set_loc)
	    {
	      /* Adjust DW_CFA_set_loc.  */
	      unsigned int cnt;
	      bfd_vma new_offset;

	      width = get_DW_EH_PE_width (ent->fde_encoding, ptr_size);
	      new_offset = ent->new_offset + 8
			   + extra_augmentation_string_bytes (ent)
			   + extra_augmentation_data_bytes (ent);

	      for (cnt = 1; cnt <= ent->set_loc[0]; cnt++)
		{
		  buf = start + ent->set_loc[cnt];

		  value = read_value (abfd, buf, width,
				      get_DW_EH_PE_signed (ent->fde_encoding));
		  if (!value)
		    continue;

		  if ((ent->fde_encoding & 0x70) == DW_EH_PE_pcrel)
		    value += (bfd_vma) ent->offset + 8 - new_offset;
		  if (ent->make_relative)
		    value -= (sec->output_section->vma
			      + sec->output_offset
			      + new_offset + ent->set_loc[cnt]);
		  write_value (abfd, buf, value, width);
		}
	    }
	}
    }

  /* FIXME: octets_per_byte.  */
  return bfd_set_section_contents (abfd, sec->output_section,
				   contents, (file_ptr) sec->output_offset,
				   sec->size);
}

/* Helper function used to sort .eh_frame_hdr search table by increasing
   VMA of FDE initial location.  */

static int
vma_compare (const void *a, const void *b)
{
  const struct eh_frame_array_ent *p = (const struct eh_frame_array_ent *) a;
  const struct eh_frame_array_ent *q = (const struct eh_frame_array_ent *) b;
  if (p->initial_loc > q->initial_loc)
    return 1;
  if (p->initial_loc < q->initial_loc)
    return -1;
  if (p->range > q->range)
    return 1;
  if (p->range < q->range)
    return -1;
  return 0;
}

/* Reorder .eh_frame_entry sections to match the associated text sections.
   This routine is called during the final linking step, just before writing
   the contents.  At this stage, sections in the eh_frame_hdr_info are already
   sorted in order of increasing text section address and so we simply need
   to make the .eh_frame_entrys follow that same order.  Note that it is
   invalid for a linker script to try to force a particular order of
   .eh_frame_entry sections.  */

bfd_boolean
_bfd_elf_fixup_eh_frame_hdr (struct bfd_link_info *info)
{
  asection *sec = NULL;
  asection *osec;
  struct eh_frame_hdr_info *hdr_info;
  unsigned int i;
  bfd_vma offset;
  struct bfd_link_order *p;

  hdr_info = &elf_hash_table (info)->eh_info;

  if (hdr_info->hdr_sec == NULL
      || info->eh_frame_hdr_type != COMPACT_EH_HDR
      || hdr_info->array_count == 0)
    return TRUE;

  /* Change section output offsets to be in text section order.  */
  offset = 8;
  osec = hdr_info->u.compact.entries[0]->output_section;
  for (i = 0; i < hdr_info->array_count; i++)
    {
      sec = hdr_info->u.compact.entries[i];
      if (sec->output_section != osec)
	{
	  _bfd_error_handler
	    (_("invalid output section for .eh_frame_entry: %pA"),
	     sec->output_section);
	  return FALSE;
	}
      sec->output_offset = offset;
      offset += sec->size;
    }


  /* Fix the link_order to match.  */
  for (p = sec->output_section->map_head.link_order; p != NULL; p = p->next)
    {
      if (p->type != bfd_indirect_link_order)
	abort();

      p->offset = p->u.indirect.section->output_offset;
      if (p->next != NULL)
	i--;
    }

  if (i != 0)
    {
      _bfd_error_handler
	(_("invalid contents in %pA section"), osec);
      return FALSE;
    }

  return TRUE;
}

/* The .eh_frame_hdr format for Compact EH frames:
   ubyte version		(2)
   ubyte eh_ref_enc		(DW_EH_PE_* encoding of typinfo references)
   uint32_t count		(Number of entries in table)
   [array from .eh_frame_entry sections]  */

static bfd_boolean
write_compact_eh_frame_hdr (bfd *abfd, struct bfd_link_info *info)
{
  struct elf_link_hash_table *htab;
  struct eh_frame_hdr_info *hdr_info;
  asection *sec;
  const struct elf_backend_data *bed;
  bfd_vma count;
  bfd_byte contents[8];
  unsigned int i;

  htab = elf_hash_table (info);
  hdr_info = &htab->eh_info;
  sec = hdr_info->hdr_sec;

  if (sec->size != 8)
    abort();

  for (i = 0; i < sizeof (contents); i++)
    contents[i] = 0;

  contents[0] = COMPACT_EH_HDR;
  bed = get_elf_backend_data (abfd);

  BFD_ASSERT (bed->compact_eh_encoding);
  contents[1] = (*bed->compact_eh_encoding) (info);

  count = (sec->output_section->size - 8) / 8;
  bfd_put_32 (abfd, count, contents + 4);
  return bfd_set_section_contents (abfd, sec->output_section, contents,
				   (file_ptr) sec->output_offset, sec->size);
}

/* The .eh_frame_hdr format for DWARF frames:

   ubyte version		(currently 1)
   ubyte eh_frame_ptr_enc	(DW_EH_PE_* encoding of pointer to start of
				 .eh_frame section)
   ubyte fde_count_enc		(DW_EH_PE_* encoding of total FDE count
				 number (or DW_EH_PE_omit if there is no
				 binary search table computed))
   ubyte table_enc		(DW_EH_PE_* encoding of binary search table,
				 or DW_EH_PE_omit if not present.
				 DW_EH_PE_datarel is using address of
				 .eh_frame_hdr section start as base)
   [encoded] eh_frame_ptr	(pointer to start of .eh_frame section)
   optionally followed by:
   [encoded] fde_count		(total number of FDEs in .eh_frame section)
   fde_count x [encoded] initial_loc, fde
				(array of encoded pairs containing
				 FDE initial_location field and FDE address,
				 sorted by increasing initial_loc).  */

static bfd_boolean
write_dwarf_eh_frame_hdr (bfd *abfd, struct bfd_link_info *info)
{
  struct elf_link_hash_table *htab;
  struct eh_frame_hdr_info *hdr_info;
  asection *sec;
  bfd_boolean retval = TRUE;

  htab = elf_hash_table (info);
  hdr_info = &htab->eh_info;
  sec = hdr_info->hdr_sec;
  bfd_byte *contents;
  asection *eh_frame_sec;
  bfd_size_type size;
  bfd_vma encoded_eh_frame;

  size = EH_FRAME_HDR_SIZE;
  if (hdr_info->u.dwarf.array
      && hdr_info->array_count == hdr_info->u.dwarf.fde_count)
    size += 4 + hdr_info->u.dwarf.fde_count * 8;
  contents = (bfd_byte *) bfd_malloc (size);
  if (contents == NULL)
    return FALSE;

  eh_frame_sec = bfd_get_section_by_name (abfd, ".eh_frame");
  if (eh_frame_sec == NULL)
    {
      free (contents);
      return FALSE;
    }

  memset (contents, 0, EH_FRAME_HDR_SIZE);
  /* Version.  */
  contents[0] = 1;
  /* .eh_frame offset.  */
  contents[1] = get_elf_backend_data (abfd)->elf_backend_encode_eh_address
    (abfd, info, eh_frame_sec, 0, sec, 4, &encoded_eh_frame);

  if (hdr_info->u.dwarf.array
      && hdr_info->array_count == hdr_info->u.dwarf.fde_count)
    {
      /* FDE count encoding.  */
      contents[2] = DW_EH_PE_udata4;
      /* Search table encoding.  */
      contents[3] = DW_EH_PE_datarel | DW_EH_PE_sdata4;
    }
  else
    {
      contents[2] = DW_EH_PE_omit;
      contents[3] = DW_EH_PE_omit;
    }
  bfd_put_32 (abfd, encoded_eh_frame, contents + 4);

  if (contents[2] != DW_EH_PE_omit)
    {
      unsigned int i;
      bfd_boolean overlap, overflow;

      bfd_put_32 (abfd, hdr_info->u.dwarf.fde_count,
		  contents + EH_FRAME_HDR_SIZE);
      qsort (hdr_info->u.dwarf.array, hdr_info->u.dwarf.fde_count,
	     sizeof (*hdr_info->u.dwarf.array), vma_compare);
      overlap = FALSE;
      overflow = FALSE;
      for (i = 0; i < hdr_info->u.dwarf.fde_count; i++)
	{
	  bfd_vma val;

	  val = hdr_info->u.dwarf.array[i].initial_loc
	    - sec->output_section->vma;
	  val = ((val & 0xffffffff) ^ 0x80000000) - 0x80000000;
	  if (elf_elfheader (abfd)->e_ident[EI_CLASS] == ELFCLASS64
	      && (hdr_info->u.dwarf.array[i].initial_loc
		  != sec->output_section->vma + val))
	    overflow = TRUE;
	  bfd_put_32 (abfd, val, contents + EH_FRAME_HDR_SIZE + i * 8 + 4);
	  val = hdr_info->u.dwarf.array[i].fde - sec->output_section->vma;
	  val = ((val & 0xffffffff) ^ 0x80000000) - 0x80000000;
	  if (elf_elfheader (abfd)->e_ident[EI_CLASS] == ELFCLASS64
	      && (hdr_info->u.dwarf.array[i].fde
		  != sec->output_section->vma + val))
	    overflow = TRUE;
	  bfd_put_32 (abfd, val, contents + EH_FRAME_HDR_SIZE + i * 8 + 8);
	  if (i != 0
	      && (hdr_info->u.dwarf.array[i].initial_loc
		  < (hdr_info->u.dwarf.array[i - 1].initial_loc
		     + hdr_info->u.dwarf.array[i - 1].range)))
	    overlap = TRUE;
	}
      if (overflow)
	_bfd_error_handler (_(".eh_frame_hdr entry overflow"));
      if (overlap)
	_bfd_error_handler (_(".eh_frame_hdr refers to overlapping FDEs"));
      if (overflow || overlap)
	{
	  bfd_set_error (bfd_error_bad_value);
	  retval = FALSE;
	}
    }

  /* FIXME: octets_per_byte.  */
  if (!bfd_set_section_contents (abfd, sec->output_section, contents,
				 (file_ptr) sec->output_offset,
				 sec->size))
    retval = FALSE;
  free (contents);

  if (hdr_info->u.dwarf.array != NULL)
    free (hdr_info->u.dwarf.array);
  return retval;
}

/* Write out .eh_frame_hdr section.  This must be called after
   _bfd_elf_write_section_eh_frame has been called on all input
   .eh_frame sections.  */

bfd_boolean
_bfd_elf_write_section_eh_frame_hdr (bfd *abfd, struct bfd_link_info *info)
{
  struct elf_link_hash_table *htab;
  struct eh_frame_hdr_info *hdr_info;
  asection *sec;

  htab = elf_hash_table (info);
  hdr_info = &htab->eh_info;
  sec = hdr_info->hdr_sec;

  if (info->eh_frame_hdr_type == 0 || sec == NULL)
    return TRUE;

  if (info->eh_frame_hdr_type == COMPACT_EH_HDR)
    return write_compact_eh_frame_hdr (abfd, info);
  else
    return write_dwarf_eh_frame_hdr (abfd, info);
}

/* Return the width of FDE addresses.  This is the default implementation.  */

unsigned int
_bfd_elf_eh_frame_address_size (bfd *abfd, const asection *sec ATTRIBUTE_UNUSED)
{
  return elf_elfheader (abfd)->e_ident[EI_CLASS] == ELFCLASS64 ? 8 : 4;
}

/* Decide whether we can use a PC-relative encoding within the given
   EH frame section.  This is the default implementation.  */

bfd_boolean
_bfd_elf_can_make_relative (bfd *input_bfd ATTRIBUTE_UNUSED,
			    struct bfd_link_info *info ATTRIBUTE_UNUSED,
			    asection *eh_frame_section ATTRIBUTE_UNUSED)
{
  return TRUE;
}

/* Select an encoding for the given address.  Preference is given to
   PC-relative addressing modes.  */

bfd_byte
_bfd_elf_encode_eh_address (bfd *abfd ATTRIBUTE_UNUSED,
			    struct bfd_link_info *info ATTRIBUTE_UNUSED,
			    asection *osec, bfd_vma offset,
			    asection *loc_sec, bfd_vma loc_offset,
			    bfd_vma *encoded)
{
  *encoded = osec->vma + offset -
    (loc_sec->output_section->vma + loc_sec->output_offset + loc_offset);
  return DW_EH_PE_pcrel | DW_EH_PE_sdata4;
}