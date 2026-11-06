#include "dare.h"
void SndManager::AddCallBack(void (*transition_callback)(int), int lParam)
{
    stCallBackRequest_ Request;
    Request.pfntransition_callback = transition_callback;
    Request.lCallBackParam = lParam;
    gs_CallBackRequestList.AddTail(Request);
}
