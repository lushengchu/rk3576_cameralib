#include "camera_main.h"

/*************************************************
 * Description: 
 * Parameter:
 * Return:
 * Note:
 *************************************************/
int sys_sem_init(void)
{
    int ret = 0;

    ret |= sem_init(&csem.PkgSem, 0, 0); 
    ret |= sem_init(&csem.camSem, 0, 0); 
    ret |= sem_init(&csem.disSem, 0, 0); 
    ret |= sem_init(&csem.aiSem, 0, 0); 
    ret |= sem_init(&csem.gasQuantificationeSem, 0, 0);

    return 0;
}

/*************************************************
 * Description: 
 * Parameter:
 * Return:
 * Note:
 *************************************************/
int sys_sem_deinit(void)
{
    int ret = 0;

    ret |= sem_destroy(&csem.PkgSem); 
    ret |= sem_destroy(&csem.camSem); 
    ret |= sem_destroy(&csem.disSem); 
    ret |= sem_destroy(&csem.aiSem); 
    ret |= sem_destroy(&csem.gasQuantificationeSem);

    return 0;
}
