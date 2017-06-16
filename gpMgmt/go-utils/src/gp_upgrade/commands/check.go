package commands

import (
	"database/sql"
	"fmt"
	"gp_upgrade/config"

	_ "github.com/lib/pq"

	"gp_upgrade/db"

	_ "github.com/mattn/go-sqlite3"
	"github.com/pkg/errors"
)

type CheckCommand struct {
	Object_count ObjectCountCommand `command:"object-count" alias:"oc" description:"count database objects and numeric objects"`
	GPDB_version VersionCommand     `command:"version" alias:"ver" description:"validate current version is upgradable"`

	Master_host string `long:"master-host" required:"no" description:"Domain name or IP of host"`
	Master_port int    `long:"master-port" required:"no" default:"15432" description:"Port for master database"`

	// for testing only, so using hidden:"true"
	Database_type   string `long:"database_type" default:"postgres" hidden:"true"`
	Database_config string `long:"database_config_file" hidden:"true"`
	Database_name   string `long:"database-name" default:"template1" hidden:"true"`
}

func (cmd CheckCommand) Execute([]string) error {
	// to work around a bug in go-flags, where an attribute is required in both parent and child command,
	// we make that attribute optional in the command struct used by go-flags
	// but enforce the requirement in our code here.
	if cmd.Master_host == "" {
		return errors.New("the required flag '--master-host' was not specified")
	}
	if cmd.Database_config == "" {
		// todo fully use dbconn; below just using it for user info
		dbconn := db.NewDBConn(cmd.Master_host, cmd.Master_port, cmd.Database_name, "", "")
		cmd.Database_config = fmt.Sprintf("host=%s port=%d user=%s "+
			"dbname=%s sslmode=disable",
			cmd.Master_host, cmd.Master_port, dbconn.User, cmd.Database_name)
	}

	connection, err := sql.Open(cmd.Database_type, cmd.Database_config)
	if err != nil {
		return err
	}
	defer connection.Close()

	rows, err := connection.Query(`select * from gp_segment_configuration, pg_filespace_entry where fsedbid = dbid`)
	if err != nil {
		return err
	}
	defer rows.Close()

	configWriter, err := config.NewWriter(rows)
	if err != nil {
		return err
	}

	err = configWriter.Write()
	return err
}