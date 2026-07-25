#ifndef JAUSOUNDTABLE_H
#define JAUSOUNDTABLE_H

#include "JSystem/JAudio2/JAISound.h"
#include "JSystem/JAudio2/JASGadget.h"
#include "helpers/endian.h"

/**
 * @ingroup jsystem-jaudio
 * 
 */
struct JAUSoundTableItem {
    u8 mPriority;
    u8 field_0x1;
    BE(u16) mResourceId;
    BE(u32) field_0x4;
    BE(f32) field_0x8;
};

/**
 * @ingroup jsystem-jaudio
 * 
 */
template<typename Root, typename Section, typename Group, typename Typename_0>
struct JAUSoundTable_ {
    JAUSoundTable_() {
        mData = NULL;
        mRoot = 0;
    }

    void reset() {
        mData = NULL;
        mRoot = NULL;
    }

    void init(const void* param_0) {
        mData = param_0;
        // magic number is not in debug rom. I'm not sure what this comparison is (maybe some sort of '' number?)
        // I also do not know how it is different between JAUSoundTable and JAUSoundNameTable
        // Future person here: This is checking for either "BST " or "BSTN", with the second two letters in Root::magicNumber().
        // Idk why the operations here are all weird but I can't use objdiff right now.
        if (*(BE(u32)*)mData + 0xbdad0000 != Root::magicNumber()) {
            mData = NULL;
        } else {
            mRoot = (Root*)((u8*)mData + *((BE(u32)*)mData + 3));
        }
    }

    Section* getSection(int index) const {
        if (index < 0) {
            return NULL;
        }
        if ((u32)index >= mRoot->mSectionNumber) {
            return NULL;
        }
        u32 offset = mRoot->mSectionOffsets[index];
        if (offset == 0) {
            return NULL;
        } 
        return (Section*)((u8*)mData + offset);
    }

    Group* getGroup(Section* param_1, int index) const {
        int iVar1;

        if (index < 0) {
            return NULL;
        } 
        if ((u32)index >= param_1->mNumGroups) {
            return NULL;
        }
        u32 offset = param_1->getGroupOffset(index);
        if (offset == 0) {
            return NULL;
        } 
        return (Group*)((u8*)mData + offset);
    }

    const void* mData;
    Root* mRoot;
};

/**
 * @ingroup jsystem-jaudio
 * 
 */
struct JAUSoundTableRoot {
    static inline u32 magicNumber() { return 'T '; } // Second half of "BST "
    BE(u32) mSectionNumber;
    BE(u32) mSectionOffsets[0];
};

/**
 * @ingroup jsystem-jaudio
 * 
 */
struct JAUSoundTableSection {
    int getGroupOffset(int index) const {
        if (index < 0) {
            return 0;
        }
        if (index >= mNumGroups) {
            return 0;
        }
        return mGroupOffsets[index];
    }

    BE(u32) mNumGroups;
    BE(u32) mGroupOffsets[0];
};

/**
 * @ingroup jsystem-jaudio
 * 
 */
struct JAUSoundTableGroup {
    u8 getTypeID(int index) const {
        if (index < 0) {
            return 0;
        }
        if (index >= mNumItems) {
            return 0xff;
        }
        return mTypeIds[index * 4];
    }

    u32 getItemOffset(int index) const {
        if (index < 0) {
            return 0;
        }
        if (index >= mNumItems) {
            return 0;
        }
        return *(BE(u32)*)(mTypeIds + index * 4) & 0xffffff;
    }

    BE(u32) mNumItems;
    BE(u32) field_0x4;
    u8 mTypeIds[0]; // TODO: Should probably be BE(u32), but I can't objdiff rn.
};

/**
 * @ingroup jsystem-jaudio
 * 
 */
struct JAUSoundTable : public JASGlobalInstance<JAUSoundTable> {
    JAUSoundTable(bool setInstance) : JASGlobalInstance<JAUSoundTable>(setInstance) {
    }
    ~JAUSoundTable() {}
    
    void init(void const*);
    u8 getTypeID(JAISoundID) const;
    JAUSoundTableItem* getData(JAISoundID) const;
    int getNumGroups_inSection(u8) const;
    int getNumItems_inGroup(u8, u8) const;

    JAUSoundTableItem* getItem(JAUSoundTableGroup* group, int index) const {
        u32 offset = group->getItemOffset(index);
        if (offset == 0) {
            return NULL;
        }
        return (JAUSoundTableItem*)((u8*)field_0x0.mData + offset);
    }

    const void* getResource() const { return field_0x0.mData; }
    bool isValid() const { return field_0x0.mData != NULL; }

    JAUSoundTable_<JAUSoundTableRoot,JAUSoundTableSection,JAUSoundTableGroup,void> field_0x0;
};

/**
 * @ingroup jsystem-jaudio
 * 
 */
struct JAUSoundNameTableRoot {
    static inline u32 magicNumber() { return 'TN'; } // Second half of "BSTN"
    BE(u32) mSectionNumber;
    BE(u32) mSectionOffsets[0];
};
/**
 * @ingroup jsystem-jaudio
 * 
 */
struct JAUSoundNameTableSection {};

/**
 * @ingroup jsystem-jaudio
 * 
 */
struct JAUSoundNameTableGroup {};

/**
 * @ingroup jsystem-jaudio
 * 
 */
struct JAUSoundNameTable : public JASGlobalInstance<JAUSoundNameTable> {
    JAUSoundNameTable(bool param_0) : JASGlobalInstance<JAUSoundNameTable>(param_0) {
    }
    ~JAUSoundNameTable() {}
    int getNumGroups_inSection(u8) const;
    int getNumItems_inGroup(u8, u8) const;
    void init(void const*);
    const char* getName(JAISoundID) const;
    const char* getGroupName(JAISoundID) const;

    JAUSoundTable_<JAUSoundNameTableRoot,JAUSoundNameTableSection,JAUSoundNameTableGroup,void> field_0x0;
};

#endif /* JAUSOUNDTABLE_H */
