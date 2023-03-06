output_log_dir="./test"
output_log_file="diffkv"

i=11
while(($i<13))
do
    echo "=====================this is ${i}th test================"
    mkdir "${output_log_dir}/${i}"
    ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KB100GB.spec  -phase load -configpath configDir/diffkv_config.ini > "${output_log_dir}/${i}/${output_log_file}load.log"
    du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}load.log"
    ./ycsbc -db diffkv -dbfilename /mnt/sdc/diffkv -threads 16 -P workloads/workloadpareto1KBcorea100GB.spec  -phase run -configpath configDir/diffkv_config.ini > "${output_log_dir}/${i}/${output_log_file}run.log"
    du -sh /mnt/sdc/diffkv >>"${output_log_dir}/${i}/${output_log_file}run.log"
    rm -rf /mnt/sdc/diffkv

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