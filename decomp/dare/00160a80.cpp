#include "dare.h"

extern "C" int CreateSema(SemaParam *);
extern int m_lock;
extern int m_lockTool;
extern int m_owner;
extern int m_ownerTool;
extern unsigned int m_count;
extern unsigned int m_countTool;

void SND_fn_vInitThreadSnd(void)
{
    SemaParam stSema;
    stSema.initCount = 1;
    stSema.maxCount = 0x7fffffff;
    m_lock = CreateSema(&stSema);
    m_lockTool = CreateSema(&stSema);
    m_owner = -1;
    m_ownerTool = -1;
    m_count = 0;
    m_countTool = 0;
    SND_fn_vEnterCriticalSectionThreadSnd_();
    EnterCriticalSectionTool_();
    MouchardThreadTool(__FILE__, 0x2a);
}
