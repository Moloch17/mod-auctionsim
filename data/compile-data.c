#include <stdio.h>
#include <dirent.h>

int main(void){
    DIR *data_dir = opendir("scans");
    if(data_dir == NULL){
        perror("opendir");
        return 1;
    }

    struct dirent *entry;
    while((entry = readdir(data_dir)) != NULL){
        if(entry->d_type != DT_REG) continue;
        printf("data/%s\n", entry->d_name);
    }
    closedir(data_dir);
    return 0;
}