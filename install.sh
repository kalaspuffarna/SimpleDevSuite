#!/bin/bash
cc -O2 -Wall -o sds sds.c -lncursesw
mkdir -p /home/$USER/.local/bin
mkdir -p /home/$USER/.local/c_bin
mv sds /home/$USER/.local/c_bin/
echo "" >> /home/$USER/.local/bin/sds 
printf "#!/bin/bash\nexec /home/$USER/.local/bin/sds \"\$@\"" > /home/$USER/.local/bin/sds
printf "#!/bin/bash\ncurrentdir=\$(pwd)\ncd /tmp\ngit clone git@github.com:kalaspuffarna/SimpleDevSuite.git\ncd SimpleDevSuite\ncc -O2 -Wall -o sds sds.c -lncursesw\nmv sds /home/$USER/.local/c_bin/sds\ncd \$currentdir\nrm -rf /tmp/SimpleDevSuite" > /home/$USER/.local/bin/sds_update
chmod +x /home/$USER/.local/bin/sds
chmod +x /home/$USER/.local/bin/sds_update
