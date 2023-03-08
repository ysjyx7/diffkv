output_log_dir="./test"
output_log_file="diffkv"

i=32
while(($i<34))
do
    echo "=====================this is ${i}th test================"
    mkdir "${output_log_dir}/${i}"
    ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KB100GB.spec  -phase load -configpath configDir/diffkv_config.ini > "${output_log_dir}/${i}/${output_log_file}loada.log"
    du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}loada.log"
    ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KBcorea100GB.spec  -phase run -configpath configDir/diffkv_config.ini > "${output_log_dir}/${i}/${output_log_file}runa.log"
    du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}runa.log"
    rm -rf /mnt/sdc/diffkv

    # ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KB100GB.spec  -phase load -configpath configDir/diffkv_config.ini > "${output_log_dir}/${i}/${output_log_file}loade.log"
    # du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}loade.log"
    # ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KBcoree100GB.spec  -phase run -configpath configDir/diffkv_config.ini > "${output_log_dir}/${i}/${output_log_file}rune.log"
    # du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}rune.log"
    # rm -rf /mnt/sdc/diffkv

    # ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KB100GB.spec  -phase load -configpath configDir/titandb_config.ini > "${output_log_dir}/${i}/${output_log_file}load_nolazymerge.log"
    # du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}load_nolazymerge.log"
    # ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KBcorea100GB.spec  -phase run -configpath configDir/titandb_config.ini > "${output_log_dir}/${i}/${output_log_file}run_nolazymerge.log"
    # du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}run_nolazymerge.log"
    # rm -rf /mnt/sdc/diffkv


    # ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KB100GB.spec  -phase load -configpath configDir/diffkv_config1.ini > "${output_log_dir}/${i}/${output_log_file}load_bt.log"
    # du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}load_bt.log"
    # ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KBcorea100GB.spec  -phase run -configpath configDir/diffkv_config1.ini > "${output_log_dir}/${i}/${output_log_file}run_bt.log"
    # du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}run_bt.log"
    # rm -rf /mnt/sdc/diffkv

    # ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KB100GB.spec  -phase load -configpath configDir/titandb_config1.ini > "${output_log_dir}/${i}/${output_log_file}load_nolazymerge_bt.log"
    # du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}load_nolazymerge_bt.log"
    # ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KBcorea100GB.spec  -phase run -configpath configDir/titandb_config1.ini > "${output_log_dir}/${i}/${output_log_file}run_nolazymerge_bt.log"
    # du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}run_nolazymerge_bt.log"
    # rm -rf /mnt/sdc/diffkv
    let "i++"
done